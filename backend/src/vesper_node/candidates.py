"""Long-term memory candidates (task 17, Part 4): the backend *proposes*, Vesper *disposes*.

After a node turn has been answered, :func:`detect` decides, with cheap deterministic patterns,
whether Kevin's words are worth proposing for Vesper's long-term memory. If they are,
:class:`CandidateQueue` appends **one** JSON line to ``~/memory/node-candidates.jsonl``
(override ``VESPER_NODE_CANDIDATES_FILE``). Vesper's main agent reviews that queue on her own
cadence (``docs/node-memory-integration.md`` §3, the review contract) and is the only writer of
curated memory. This module never reads or writes ``MEMORY.md`` or any other memory file.

Safety (non-negotiable, task-16 review): this module imports **only the standard library**.
It does not import the brain client, ``httpx``, the registry or anything else that can act,
so no memory code path can reach home control. Detection is pattern matching, never a brain
``/ask`` (``/ask`` always runs the home-control tool loop). A test enforces the import rule.

Significance (bias hard toward *not* queueing: a missed memory is a minor gap, a wrong one
pollutes the curated store):

* **explicit**: a sentence that *starts* with an imperative trigger: "remember (that|this|to)
  …", "don't forget …", "save that", "note that …", "make a note …", "keep in mind …". A bare
  "remember that" points at the preceding sentence of the same utterance, or else at the
  previous exchange of this node's session.
* **heuristic**: a short, declarative, first-person sentence matching a tight pattern: a
  preference ("I like my coffee black"), a favourite, a birthday/anniversary, an allergy or
  diet, a reminder request ("remind me to call the plumber"), a dated appointment.
* **never**: questions, follow-ups ("tell me more"), small talk, transient state (weather,
  "right now"), device commands, hedged or hypothetical sentences, short-term timers, and
  anything that is not clearly Kevin's own statement (reported speech: "my wife said …",
  "he told me …", "she wants …").

Privacy: a candidate carries the single significant exchange, trimmed to the significant
sentence(s) and capped at :data:`MAX_TEXT_CHARS` per side. Never audio, never the transcript.
Text is never logged: logs carry node ids, significance and fixed reason labels only.

Queue file: mode 600 (enforced on an existing file), parent created 700 if missing (an
existing parent is never chmod-ed), opened ``O_APPEND|O_NOFOLLOW``, one ``write`` per line
under a thread lock **and** an exclusive ``flock`` on the file, so Vesper's rotation (see the
review contract) can take the same lock. If the path no longer names the inode we locked
(Vesper rotated it), the append is retried on the new file.
"""

from __future__ import annotations

import errno
import fcntl
import hashlib
import json
import logging
import os
import re
import secrets
import stat
import threading
from collections.abc import Callable
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any

log = logging.getLogger("vesper_node.candidates")

EXPLICIT = "explicit"
HEURISTIC = "heuristic"
PENDING = "pending"
MAX_TEXT_CHARS = 300  # per side (user_text, vesper_reply)
MAX_HEURISTIC_WORDS = 30  # a heuristic sentence longer than this is "ambiguous"
MAX_UTTERANCE_WORDS = 80  # a rambling utterance never yields a heuristic candidate
PENDING_WARN = 500  # spec §6: a queue this long means the review loop is stuck
CANDIDATE_KEYS = (
    "id",
    "ts",
    "node_id",
    "room",
    "user_text",
    "vesper_reply",
    "significance",
    "reason",
    "status",
    "dedupe_key",
)


class CandidateError(RuntimeError):
    """The queue file could not be appended to. The message is a short label."""


@dataclass(frozen=True)
class Significance:
    kind: str  # EXPLICIT | HEURISTIC
    reason: str  # fixed label text (never derived from the transcript)
    user_text: str  # the significant sentence(s), trimmed
    vesper_reply: str
    core: str  # the fact itself, for the dedupe key


# ---- text helpers -------------------------------------------------------------------------

_WS = re.compile(r"\s+")
_QUOTES = str.maketrans({"’": "'", "‘": "'", "“": '"', "”": '"'})
_SENTENCE_SPLIT = re.compile(r"(?<=[.!?])\s+|\s*;\s*")
_LEAD_FILLER = re.compile(
    r"^(?:(?:hey|ok|okay|so|um|uh|oh|and|also|alright|right|vesper|by the way|btw|fyi|"
    r"just so you know|for the record)\b[,!:.]?\s*)+",
    re.IGNORECASE,
)
_TRAIL_PUNCT = " .,!;:-"


def _squash(text: str) -> str:
    return _WS.sub(" ", text.translate(_QUOTES)).strip()


def cap(text: str, limit: int = MAX_TEXT_CHARS) -> str:
    """Collapse whitespace; over ``limit``, cut at a word boundary and add an ellipsis."""
    text = _squash(text)
    if len(text) <= limit:
        return text
    cut = text[: limit - 1]
    if " " in cut[limit // 2 :]:
        cut = cut[: cut.rfind(" ")]
    return cut.rstrip(_TRAIL_PUNCT) + "…"


def sentences(text: str) -> list[str]:
    return [s for s in (p.strip() for p in _SENTENCE_SPLIT.split(_squash(text))) if s]


def _words(text: str) -> list[str]:
    return re.findall(r"[a-z0-9']+", text.lower())


def _strip_filler(text: str) -> str:
    return _LEAD_FILLER.sub("", text).strip()


def dedupe_key(core: str) -> str:
    """Hash of the normalised core fact: case, punctuation, apostrophes and a leading
    "that" do not matter, so "Remember that I like my coffee black." and "I like my coffee
    black" collide."""
    norm = " ".join(_words(core.replace("'", "")))
    norm = re.sub(r"^(?:that|this) ", "", norm)
    return "sha256:" + hashlib.sha256(norm.encode()).hexdigest()[:32]


# ---- exclusion rules ----------------------------------------------------------------------

# Reported speech / other people's statements: never Kevin's own (spec §2, §5).
_REPORTED = re.compile(
    r"\b(?:said|says|saying|told|tells|telling|mentioned|mentions|claims?|claimed|"
    r"according\s+to|asked\s+me|asks\s+me|apparently|supposedly|"
    r"(?:she|he|they)\s+(?:wants?|wanted|likes?|liked|loves?|loved|prefers?|preferred|"
    r"hates?|hated|needs?|needed|thinks?|thought|believes?|feels?|felt|wish(?:es)?|"
    r"hopes?|promised))\b",
    re.IGNORECASE,
)
# Third-person attitude verbs ("my wife wants", "Charity likes"): someone else's preference.
_THIRD_PERSON_ATTITUDE = re.compile(
    r"\b(?:wants|likes|loves|prefers|hates|thinks|believes|feels|wishes|hopes|"
    r"doesn't\s+(?:like|want|eat|drink)|does\s+not\s+(?:like|want|eat|drink))\b",
    re.IGNORECASE,
)
# Hedged / hypothetical / joking: ambiguous, never a heuristic candidate.
_HEDGE = re.compile(
    r"\b(?:maybe|probably|perhaps|possibly|might|i\s+think|i\s+guess|i\s+suppose|not\s+sure|"
    r"kind\s+of|sort\s+of|kinda|sorta|sometimes|if|unless|would|could|should|wish|joking|"
    r"kidding|pretend|imagine|hypothetically|lol)\b",
    re.IGNORECASE,
)
# Transient state: the weather, the time, how things are right now.
_TRANSIENT = re.compile(
    r"\b(?:weather|rain(?:ing|y)?|snow(?:ing|y)?|sunny|cloudy|windy|temperature|degrees|"
    r"right\s+now|at\s+the\s+moment|currently|today|tonight|"
    r"this\s+(?:morning|afternoon|evening|week)|o'clock)\b",
    re.IGNORECASE,
)
# Deictic objects point into the conversation, not at a durable fact ("I like that").
_DEICTIC = re.compile(
    r"\b(?:it|that|this|these|those|you|your|them|him|her|here|there|one)\b", re.IGNORECASE
)
_QUESTION_OPENER = re.compile(
    r"^(?:what|what's|whats|who|who's|when|where|where's|why|how|how's|which|whose|is|are|"
    r"am|was|were|do|does|did|can|could|would|will|should|shall|may|have|has|had|isn't|"
    r"aren't|don't|doesn't|didn't|won't|wouldn't|tell\s+me|any|anything)\b",
    re.IGNORECASE,
)
# Device / home-control commands: an action, not a fact.
_DEVICE_COMMAND = re.compile(
    r"^(?:please\s+)?(?:turn|switch|set|dim|brighten|open|close|shut|lock|unlock|play|pause|"
    r"resume|stop|start|skip|raise|lower|increase|decrease|mute|unmute|volume|toggle|"
    r"arm|disarm|run|activate|deactivate|enable|disable)\b",
    re.IGNORECASE,
)
# "in 10 minutes", "in an hour": a kitchen timer, not a durable reminder.
_SHORT_TIMER = re.compile(
    r"\bin\s+(?:a|an|one|two|three|four|five|ten|fifteen|twenty|thirty|a\s+few|a\s+couple"
    r"(?:\s+of)?|\d+)\s+(?:seconds?|secs?|minutes?|mins?|hours?|hrs?)\b",
    re.IGNORECASE,
)


def _reported(text: str) -> bool:
    return bool(_REPORTED.search(text) or _THIRD_PERSON_ATTITUDE.search(text))


def is_question(text: str) -> bool:
    text = _strip_filler(_squash(text))
    return "?" in text or bool(_QUESTION_OPENER.match(text))


def is_device_command(text: str) -> bool:
    return bool(_DEVICE_COMMAND.match(_strip_filler(_squash(text))))


# ---- explicit triggers --------------------------------------------------------------------

_POLITE = r"(?:(?:please\s+)?(?:(?:can|could|would|will)\s+you\s+)?(?:please\s+)?)"
_TRIGGER = re.compile(
    rf"^{_POLITE}(?P<trigger>remember|don't\s+forget|dont\s+forget|do\s+not\s+forget|"
    r"never\s+forget|save|note|make\s+(?:a\s+)?note(?:\s+of)?|take\s+(?:a\s+)?note(?:\s+of)?|"
    r"keep\s+in\s+mind)\b[,:]?\s*(?P<rest>.*)$",
    re.IGNORECASE,
)
_POLITE_LEAD = re.compile(r"^(?:please\s+)?(?:can|could|would|will)\s+you\b", re.IGNORECASE)
# "this", "that", "it" (+ "for me", "please", "to memory"): points at something else.
_BARE = re.compile(
    r"^(?:this|that|it|all\s+that|this\s+one|that\s+one)?\s*(?:for\s+me|for\s+later|please|"
    r"(?:to|in)\s+(?:your\s+)?memory|for\s+next\s+time)?\s*(?:,?\s*please)?$",
    re.IGNORECASE,
)
# "this: X" / "that, X": the payload follows a colon or comma.
_POINTER_THEN_PAYLOAD = re.compile(r"^(?:this|that)\s*[:,\-]\s*(?P<payload>.+)$", re.IGNORECASE)
# Recall questions and reminiscing: "remember when we …", "remember what I said".
_RECALL = re.compile(
    r"^(?:when|what|who|whom|where|why|how|which|if|whether|me|us|the\s+time|the\s+day|"
    r"anything|everything|nothing)\b",
    re.IGNORECASE,
)

_TRIGGER_LABEL = {
    "remember": "remember",
    "dont forget": "don't forget",
    "don't forget": "don't forget",
    "do not forget": "don't forget",
    "never forget": "don't forget",
    "save": "save that",
    "note": "note that",
    "make note": "make a note",
    "make a note": "make a note",
    "make note of": "make a note",
    "make a note of": "make a note",
    "take note": "make a note",
    "take a note": "make a note",
    "take note of": "make a note",
    "take a note of": "make a note",
    "keep in mind": "keep in mind",
}


@dataclass(frozen=True)
class _Explicit:
    reason: str  # fixed label text
    payload: str | None  # None: bare pointer ("remember that")
    reminder: bool = False


def _explicit(sentence: str) -> _Explicit | None:
    """Parse one sentence that may start with an explicit trigger."""
    lead = _strip_filler(sentence)
    m = _TRIGGER.match(lead.rstrip(_TRAIL_PUNCT + "?"))
    if m is None:
        return None
    if "?" in sentence and not _POLITE_LEAD.match(lead):
        return None  # "remember that?" is a recall question
    trigger = _WS.sub(" ", m.group("trigger").lower())
    label = _TRIGGER_LABEL.get(trigger, trigger)
    rest = m.group("rest").strip(_TRAIL_PUNCT)
    low = rest.lower()
    narrow = trigger in ("save", "note") or trigger.startswith(("make", "take"))
    if _BARE.fullmatch(rest):
        if not rest:
            return None  # a lone "remember" / "save" may be a cut-off utterance: too vague
        return _Explicit(f"user said '{label}' (pointing at the exchange)", None)
    pointed = _POINTER_THEN_PAYLOAD.match(rest)
    if pointed:
        return _Explicit(f"user said '{label}'", pointed.group("payload").strip(_TRAIL_PUNCT))
    if low.startswith("that "):
        payload = rest[5:].strip()
        if trigger == "save" or len(_words(payload)) < 2:
            return None  # "save that file", "remember that song": a noun phrase
        return _Explicit(f"user said '{label} that'", payload)
    if low.startswith("to ") and trigger != "save" and trigger != "note":
        payload = rest[3:].strip()
        if not _words(payload):
            return None
        return _Explicit(f"user said '{label} to' (reminder)", payload, reminder=True)
    if narrow:
        return None  # "save the file", "note the time", "make a note of the time"
    if low.startswith("about "):
        payload = rest[6:].strip()
        return _Explicit(f"user said '{label} about'", payload) if _words(payload) else None
    if _RECALL.match(rest) or len(_words(rest)) < 2:
        return None  # "remember when we …", "remember me"
    return _Explicit(f"user said '{label}'", rest)


# ---- heuristics ---------------------------------------------------------------------------

_I = r"(?:i|i'm|im|i\s+am)"
_HEURISTICS: tuple[tuple[re.Pattern[str], str, bool], ...] = (
    # (pattern on the sentence, reason label, object must be non-deictic + non-transient)
    (
        re.compile(
            r"^i\s+(?:really\s+|actually\s+|always\s+|absolutely\s+)?(?:like|love|prefer|hate|"
            r"dislike|enjoy|can't\s+stand|cant\s+stand|don't\s+like|do\s+not\s+like)\s+"
            r"(?P<obj>.+)$",
            re.IGNORECASE,
        ),
        "durable preference",
        True,
    ),
    (
        re.compile(
            r"^my\s+(?:all[- ]time\s+)?favou?rite\s+[a-z' ]{1,30}?\s+(?:is|are)\s+(?P<obj>.+)$",
            re.IGNORECASE,
        ),
        "favourite",
        True,
    ),
    (
        re.compile(
            r"^(?:my|our|(?:my\s+)?[a-z]+'s)\s+(?:wedding\s+)?(?:birthday|anniversary)\s+"
            r"(?:is|falls\s+on)\s+(?P<obj>.+)$",
            re.IGNORECASE,
        ),
        "birthday or anniversary",
        False,
    ),
    (
        re.compile(rf"^{_I}\s+(?:allergic|intolerant)\s+to\s+(?P<obj>.+)$", re.IGNORECASE),
        "allergy",
        True,
    ),
    (
        re.compile(
            rf"^{_I}\s+(?:a\s+)?(?P<obj>vegetarian|vegan|pescatarian|gluten[- ]free|"
            r"lactose[- ]intolerant|teetotal)$",
            re.IGNORECASE,
        ),
        "diet",
        False,
    ),
    (
        re.compile(r"^i\s+(?:don't|do\s+not|never)\s+(?:eat|drink)\s+(?P<obj>.+)$", re.IGNORECASE),
        "diet",
        True,
    ),
    (
        re.compile(
            rf"^{_POLITE}remind\s+me\s+to\s+(?P<obj>.+)$",
            re.IGNORECASE,
        ),
        "reminder request",
        False,
    ),
    (
        re.compile(
            r"^(?:i|we)\s+(?:have|'ve\s+got|have\s+got)\s+(?:a|an|my|our)\s+"
            r"(?:[a-z'-]+\s+){0,2}(?:appointment|meeting|flight|interview|reservation|"
            r"surgery|checkup|check-up|party|wedding)\s+"
            r"(?P<obj>(?:on|at|next|this\s+(?:coming\s+)?(?:monday|tuesday|wednesday|thursday|"
            r"friday|saturday|sunday)|tomorrow|in\s+(?:january|february|march|april|may|june|"
            r"july|august|september|october|november|december))\b.*)$",
            re.IGNORECASE,
        ),
        "upcoming event",
        False,
    ),
    (
        re.compile(
            r"^my\s+(?:shoe\s+size|blood\s+type|middle\s+name|dentist|doctor|"
            r"(?:car|license|licence)\s+plate)\s+is\s+(?P<obj>.+)$",
            re.IGNORECASE,
        ),
        "personal detail",
        True,
    ),
)


def _heuristic(sentence: str) -> str | None:
    """The reason label if ``sentence`` is a tight, unhedged, durable first-person fact."""
    s = _strip_filler(sentence).rstrip(_TRAIL_PUNCT)
    if not s or "?" in sentence or len(_words(s)) > MAX_HEURISTIC_WORDS:
        return None
    if _HEDGE.search(s) or is_device_command(s) or _QUESTION_OPENER.match(s):
        return None
    for pattern, label, strict in _HEURISTICS:
        m = pattern.match(s)
        if m is None:
            continue
        obj = m.group("obj").strip()
        if not _words(obj):
            return None
        if label == "reminder request" and _SHORT_TIMER.search(obj):
            return None  # "remind me to check the oven in 10 minutes": a timer
        if strict and (_DEICTIC.search(obj) or _TRANSIENT.search(obj)):
            return None  # "I like that", "I love this weather"
        if obj.lower().startswith(("when ", "how ", "what ", "where ", "the way ")):
            return None
        return label
    return None


# ---- detection ----------------------------------------------------------------------------


def detect(
    user_text: str,
    vesper_reply: str,
    *,
    previous: tuple[str, str] | None = None,
) -> Significance | None:
    """Is this exchange worth proposing for long-term memory? Pure, deterministic, cheap.

    ``previous`` is this node's previous exchange in the current session (user, reply); it is
    used only when Kevin says a bare "remember that" with nothing else in the utterance.
    """
    if not user_text or not user_text.strip():
        return None
    parts = sentences(user_text)
    if not parts:
        return None
    # explicit: any sentence that starts with a trigger
    for i, sentence in enumerate(parts):
        ex = _explicit(sentence)
        if ex is None:
            continue
        if ex.payload is not None:
            if _reported(ex.payload) or "?" in ex.payload:
                return None
            if not ex.reminder and is_question(ex.payload):
                return None
            core = ("remind: " if ex.reminder else "") + ex.payload
            return Significance(EXPLICIT, ex.reason, cap(sentence), cap(vesper_reply), core)
        # bare pointer: the preceding sentence, else the next one, else the previous exchange
        neighbour = parts[i - 1] if i > 0 else (parts[i + 1] if i + 1 < len(parts) else None)
        if neighbour is not None:
            if _reported(neighbour) or is_question(neighbour) or is_device_command(neighbour):
                return None
            ordered = [neighbour, sentence] if i > 0 else [sentence, neighbour]
            text = " ".join(ordered)
            return Significance(EXPLICIT, ex.reason, cap(text), cap(vesper_reply), neighbour)
        if previous is None:
            return None  # "remember that" with nothing to point at
        prev_user, prev_reply = previous
        if not prev_user.strip() or _reported(prev_user) or is_device_command(prev_user):
            return None
        reason = ex.reason.replace("the exchange", "the previous exchange")
        core = prev_user + " / " + prev_reply
        return Significance(EXPLICIT, reason, cap(prev_user), cap(prev_reply), core)
    # heuristic: tight patterns only, on a short, unambiguous utterance
    if (
        len(_words(user_text)) > MAX_UTTERANCE_WORDS
        or "?" in user_text
        or _reported(user_text)
        or is_question(parts[0])
    ):
        return None
    for sentence in parts:
        label = _heuristic(sentence)
        if label is not None:
            core = _strip_filler(sentence)
            return Significance(HEURISTIC, label, cap(sentence), cap(vesper_reply), core)
    return None


def _now_iso() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


def build_candidate(
    sig: Significance, *, node_id: str, room: str | None, now: Callable[[], str] = _now_iso
) -> dict[str, Any]:
    """The queue line (spec §2 schema, plus ``id`` so Vesper's decisions can refer to it)."""
    return {
        "id": secrets.token_hex(8),
        "ts": now(),
        "node_id": node_id,
        "room": room,
        "user_text": sig.user_text,
        "vesper_reply": sig.vesper_reply,
        "significance": sig.kind,
        "reason": sig.reason,
        "status": PENDING,
        "dedupe_key": dedupe_key(sig.core),
    }


# ---- the queue ----------------------------------------------------------------------------


class CandidateQueue:
    """Append-only candidate queue. ``path=None`` keeps candidates in RAM (tests)."""

    def __init__(
        self,
        path: Path | str | None,
        *,
        warn_at: int = PENDING_WARN,
        now: Callable[[], str] = _now_iso,
    ) -> None:
        self.path = Path(path).expanduser() if path is not None else None
        self.warn_at = warn_at
        self.now = now
        self.staged: list[dict[str, Any]] = []  # RAM mode only
        self._lock = threading.Lock()

    def consider(
        self,
        *,
        node_id: str,
        room: str | None,
        user_text: str,
        vesper_reply: str,
        previous: tuple[str, str] | None = None,
    ) -> dict[str, Any] | None:
        """Detect and, if significant, stage. Blocking: call through ``asyncio.to_thread``.

        Never raises for an I/O problem (logged by label, never by content)."""
        sig = detect(user_text, vesper_reply, previous=previous)
        if sig is None:
            return None
        candidate = build_candidate(sig, node_id=node_id, room=room, now=self.now)
        try:
            lines = self.append(candidate)
        except (OSError, CandidateError) as e:
            log.error(
                "memory candidate not staged: node=%s reason=%s",
                node_id,
                e if isinstance(e, CandidateError) else type(e).__name__,
            )
            return None
        log.info(
            "memory candidate staged: node=%s significance=%s chars=%d",
            node_id,
            sig.kind,
            len(sig.user_text),
        )
        if lines > self.warn_at:
            log.warning(
                "memory candidate queue has %d entries (> %d): is Vesper's review stuck?",
                lines,
                self.warn_at,
            )
        return candidate

    def append(self, candidate: dict[str, Any]) -> int:
        """Append one candidate; returns the number of lines now in the queue."""
        line = (json.dumps(candidate, ensure_ascii=False, separators=(",", ":")) + "\n").encode()
        with self._lock:
            if self.path is None:
                self.staged.append(dict(candidate))
                return len(self.staged)
            return self._append_file(line)

    def _append_file(self, line: bytes) -> int:
        assert self.path is not None
        parent = self.path.parent
        if not parent.exists():
            parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        flags = os.O_WRONLY | os.O_APPEND | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC
        for _ in range(3):
            try:
                fd = os.open(self.path, flags, 0o600)
            except OSError as e:
                if e.errno == errno.ELOOP or isinstance(e, IsADirectoryError):
                    raise CandidateError("not_a_regular_file") from None
                raise
            try:
                st = os.fstat(fd)
                if not stat.S_ISREG(st.st_mode):
                    raise CandidateError("not_a_regular_file")
                fcntl.flock(fd, fcntl.LOCK_EX)
                try:
                    try:
                        cur = os.stat(self.path, follow_symlinks=False)
                    except FileNotFoundError:
                        cur = None
                    if cur is None or (cur.st_dev, cur.st_ino) != (st.st_dev, st.st_ino):
                        continue  # rotated under us: append to the new file
                    if stat.S_IMODE(st.st_mode) & 0o077:
                        os.fchmod(fd, 0o600)
                        log.warning("memory candidate queue mode tightened to 600")
                    view = memoryview(line)
                    while view:
                        view = view[os.write(fd, view) :]
                    os.fsync(fd)
                    return self._count_lines()
                finally:
                    fcntl.flock(fd, fcntl.LOCK_UN)
            finally:
                os.close(fd)
        raise CandidateError("queue_file_moving")

    def _count_lines(self) -> int:
        """Lines in the queue file itself (the backend's own file; for the §6 warning)."""
        assert self.path is not None
        fd = os.open(self.path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
        count = 0
        with os.fdopen(fd, "rb") as fh:
            while chunk := fh.read(1 << 16):
                count += chunk.count(b"\n")
        return count
