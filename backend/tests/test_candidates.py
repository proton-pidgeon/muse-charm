"""Task 17 (Part 4): long-term memory candidates: detection, queue file, async hook, safety."""

from __future__ import annotations

import ast
import asyncio
import json
import logging
import stat
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any

import httpx
import pytest
from conftest import NODE_ID, ROOM, client_for, make_note, make_settings, parse_sse, turn_headers

import vesper_node
from vesper_node import candidates as cand_mod
from vesper_node.app import create_app
from vesper_node.candidates import (
    CANDIDATE_KEYS,
    EXPLICIT,
    HEURISTIC,
    MAX_TEXT_CHARS,
    CandidateQueue,
    build_candidate,
    dedupe_key,
    detect,
)
from vesper_node.conversation import ConversationStore

SRC = Path(vesper_node.__file__).parent
PREV = ("What's the gate code", "The gate code is 4821.")

# ---- detection: explicit --------------------------------------------------------------------

EXPLICIT_CASES = [
    "remember that I like my coffee black",
    "Remember that I like my coffee black.",
    "Hey Vesper, remember that I like my coffee black",
    "Vesper, please remember that the recycling goes out on Thursdays",
    "remember this: my locker code is 42",
    "Remember this, the spare key is under the blue pot",
    "don't forget that the boiler service is due in March",
    "Don't forget to call the plumber",
    "do not forget to renew the car insurance",
    "remember to turn off the lights",  # a reminder: staged as a proposal, never executed
    "remember to do the dishes",
    "note that the gate code changed to 1234",
    "make a note that the dentist moved to Elm Street",
    "make a note to book the MOT",
    "keep in mind that I take my tea with honey",
    "Can you remember that I prefer oat milk?",
    "could you please remember that Charity's birthday is May 4th",
    "remember the milk on the way home",
    "don't forget about the parent teacher meeting",
    "Good morning. Remember that I'm off work on Friday.",
]


@pytest.mark.parametrize("text", EXPLICIT_CASES)
def test_explicit_triggers_always_queue(text: str) -> None:
    sig = detect(text, "Got it.")
    assert sig is not None and sig.kind == EXPLICIT, text


@pytest.mark.parametrize(
    "text",
    ["remember that", "Remember this.", "save that", "Don't forget that!", "note that", "save it"],
)
def test_bare_pointer_uses_previous_exchange(text: str) -> None:
    sig = detect(text, "Okay, saved.", previous=PREV)
    assert sig is not None and sig.kind == EXPLICIT
    assert (sig.user_text, sig.vesper_reply) == PREV  # the referenced single exchange
    assert sig.reason.endswith("(pointing at the previous exchange: Vesper's answer)")


def test_bare_pointer_without_previous_exchange_is_not_staged() -> None:
    assert detect("remember that", "Okay.") is None


def test_bare_pointer_at_a_sentence_in_the_same_utterance() -> None:
    sig = detect("I like my coffee black. Remember that.", "Noted.", previous=PREV)
    assert sig is not None and sig.kind == EXPLICIT
    assert sig.user_text == "I like my coffee black. Remember that."
    assert "4821" not in sig.user_text + sig.vesper_reply
    sig = detect("Remember this. The spare key is in the shed.", "Noted.")
    assert sig is not None and "spare key" in sig.user_text


@pytest.mark.parametrize(
    "prev_user",
    [
        "turn off the kitchen lights",  # device command: nothing to remember
        "my wife said the party is on Saturday",  # reported speech
        "   ",
    ],
)
def test_bare_pointer_at_an_ineligible_previous_exchange(prev_user: str) -> None:
    assert detect("remember that", "Okay.", previous=(prev_user, "Done.")) is None


def test_explicit_trimmed_to_the_significant_sentence() -> None:
    text = (
        "Morning Vesper. The game last night was something else, what a finish. "
        "Remember that I like my coffee black. Anyway, what's on today"
    )
    sig = detect(text, "Noted. " + "x" * 2000)
    assert sig is not None
    assert sig.user_text == "Remember that I like my coffee black."
    assert len(sig.vesper_reply) <= MAX_TEXT_CHARS


def test_long_text_capped() -> None:
    sig = detect("remember that " + "I like very long sentences " * 40, "ok " * 400)
    assert sig is not None
    assert len(sig.user_text) <= MAX_TEXT_CHARS and len(sig.vesper_reply) <= MAX_TEXT_CHARS


# ---- detection: heuristic -------------------------------------------------------------------

HEURISTIC_CASES = [
    "I like my coffee black",
    "I really love jazz",
    "I prefer oat milk in my tea",
    "I can't stand cilantro",
    "My favorite color is blue",
    "My favourite band is Radiohead",
    "Charity's birthday is next week",
    "My wife's birthday is March 3rd",
    "Our anniversary is June 12th",
    "I'm allergic to peanuts",
    "I am vegetarian",
    "I don't eat pork",
    "remind me to call the plumber",
    "please remind me to renew my passport next month",
    "I have a dentist appointment next Tuesday",
    "We have a flight on the 14th",
    "I have a meeting on Friday",
    "I have a job interview March 3rd",
    "We have a reservation tomorrow",
    "My shoe size is 11",
]


@pytest.mark.parametrize("text", HEURISTIC_CASES)
def test_heuristic_durable_facts_queue(text: str) -> None:
    sig = detect(text, "Noted.")
    assert sig is not None and sig.kind == HEURISTIC, text


# ---- detection: never -----------------------------------------------------------------------

NEVER_CASES = [
    # small talk
    "hello",
    "good morning",
    "good night Vesper",
    "thanks",
    "okay thanks",
    "you're funny",
    "how are you",
    "I'm fine",
    "I'm tired",
    "I'm home",
    "nice",
    # follow-ups
    "tell me more",
    "tell me more about that",
    "what about tomorrow",
    "and then what",
    "go on",
    "say that again",
    "what did you just say",
    # questions / recall
    "what's the weather",
    "what time is it",
    "do you remember what I said?",
    "do you remember my coffee order",
    "remember when we went to Paris",
    "remember what I told you yesterday",
    "remember that?",
    "remember me",
    "is the office light on",
    "what's my favorite color",
    "can you remind me what I said",
    "who won the game",
    # transient state
    "it's cold in here",
    "it's raining",
    "I love this weather",
    "I like it warm today",
    "I'm hungry right now",
    "the kitchen is a mess",
    # device commands
    "turn off the kitchen lights",
    "turn on the lights",
    "set the thermostat to 70",
    "dim the lights",
    "lock the front door",
    "play some jazz",
    "stop",
    "save the file",
    "note the time",
    "make a note of the time",
    "save that file to the desktop",
    # deictic / conversational
    "I love you",
    "I like that",
    "I like it",
    "I love this song",
    "I like the blue one",
    "I hate it when that happens",
    "I like how you said that",
    "I like your voice",
    "remember that song",
    # hedged / hypothetical / jokes
    "I think I like jazz",
    "maybe I like jazz",
    "I probably prefer tea",
    "if I liked fish I'd eat it",
    "I would like a coffee",
    "I'd like the lights off",
    "I sometimes like tea",
    "just kidding, I love broccoli",
    # undated events: no date anchor
    "I have a meeting at 3",
    "I have a meeting at 3 on Friday",
    "I have a meeting tonight",
    "I have a meeting in the office",
    "I have a dentist appointment on my calendar",
    # a question folded into the sentence
    "Hey Vesper I like my coffee black what's the time",
    "I like jazz who is playing tonight",
    "my favorite band is Radiohead when do they tour",
    "remind me to call the plumber how late are they open",
    "I prefer oat milk can you order some",
    # timers
    "remind me to check the oven in 10 minutes",
    "remind me to take the pizza out in five minutes",
    "remind me in an hour",
    # ambiguous / lone triggers
    "remember",
    "don't forget",
    "save",
    "note",
    "",
    "   ",
]


@pytest.mark.parametrize("text", NEVER_CASES)
def test_never_queue(text: str) -> None:
    assert detect(text, "Sure.") is None, text


THIRD_PARTY_CASES = [
    "my wife said she wants a new couch",
    "my wife says the party is on Saturday",
    "he told me the game is on Friday",
    "she wants a red bike for her birthday",
    "Charity likes tulips",
    "my son thinks pizza is a vegetable",
    "they said the plumber is coming Tuesday",
    "remember that my wife wants a new couch",
    "remember that she said the boiler is broken",
    "don't forget that he told me to call back",
    "remember that my boss mentioned a raise",
    "according to Dave the car needs tyres",
    "apparently the neighbours are moving",
    "remember that she prefers window seats",
    "note that Charity hates mushrooms",
    "my brother asked me to lend him the drill",
    "he wanted me to remember his number",
]


@pytest.mark.parametrize("text", THIRD_PARTY_CASES)
def test_reported_speech_and_third_party_statements_never_queue(text: str) -> None:
    assert detect(text, "Okay.") is None, text


def test_dedupe_key_normalises_the_core_fact() -> None:
    a = detect("Remember that I like my coffee black.", "Noted.")
    b = detect("hey vesper remember that i like my coffee black", "Got it, black coffee.")
    c = detect("I like my coffee black", "Noted.")
    d = detect("remember that I like my coffee white", "Noted.")
    assert a and b and c and d
    assert dedupe_key(a.core) == dedupe_key(b.core) == dedupe_key(c.core)
    assert dedupe_key(a.core) != dedupe_key(d.core)
    assert dedupe_key(a.core).startswith("sha256:")


# ---- the queue file -------------------------------------------------------------------------


def _candidate(**kw: Any) -> dict[str, Any]:
    sig = detect(kw.pop("text", "remember that I like my coffee black"), "Noted.")
    assert sig is not None
    return build_candidate(sig, node_id=NODE_ID, room=ROOM, now=lambda: "2026-10-08T15:30:00-05:00")


def _lines(path: Path) -> list[dict[str, Any]]:
    return [json.loads(line) for line in path.read_text().splitlines()]


def test_queue_schema_mode_and_parent(tmp_path) -> None:
    path = tmp_path / "home" / "memory" / "node-candidates.jsonl"
    q = CandidateQueue(path, now=lambda: "2026-10-08T15:30:00-05:00")
    got = q.consider(
        node_id=NODE_ID,
        room=ROOM,
        user_text="remember that I like my coffee black",
        vesper_reply="Got it, black coffee, noted.",
    )
    assert got is not None
    (line,) = _lines(path)
    assert tuple(line) == CANDIDATE_KEYS
    assert line == {
        "id": got["id"],
        "ts": "2026-10-08T15:30:00-05:00",
        "node_id": NODE_ID,
        "room": ROOM,
        "user_text": "remember that I like my coffee black",
        "vesper_reply": "Got it, black coffee, noted.",
        "significance": "explicit",
        "reason": "user said 'remember that'",
        "status": "pending",
        "dedupe_key": dedupe_key("I like my coffee black"),
        "speaker": "unverified",
    }
    assert stat.S_IMODE(path.stat().st_mode) == 0o600
    assert stat.S_IMODE(path.parent.stat().st_mode) == 0o700


def test_queue_is_append_only(tmp_path) -> None:
    path = tmp_path / "q.jsonl"
    path.write_text('{"preexisting": true}\n')
    path.chmod(0o600)
    q = CandidateQueue(path)
    q.append(_candidate())
    q.append(_candidate(text="I'm allergic to peanuts"))
    lines = _lines(path)
    assert lines[0] == {"preexisting": True} and len(lines) == 3
    assert lines[2]["significance"] == "heuristic"


def test_existing_parent_mode_untouched_and_loose_file_tightened(tmp_path, caplog) -> None:
    parent = tmp_path / "memory"
    parent.mkdir(mode=0o755)
    parent.chmod(0o755)
    path = parent / "node-candidates.jsonl"
    path.write_text("")
    path.chmod(0o644)
    CandidateQueue(path).append(_candidate())
    assert stat.S_IMODE(parent.stat().st_mode) == 0o755
    assert stat.S_IMODE(path.stat().st_mode) == 0o600


def test_symlinked_queue_is_refused_and_swallowed(tmp_path, caplog) -> None:
    target = tmp_path / "elsewhere.jsonl"
    target.write_text("")
    path = tmp_path / "q.jsonl"
    path.symlink_to(target)
    q = CandidateQueue(path)
    caplog.set_level(logging.INFO)
    got = q.consider(
        node_id=NODE_ID, room=ROOM, user_text="remember that I like tea", vesper_reply="Ok."
    )
    assert got is None and target.read_text() == ""
    assert "memory candidate not staged" in caplog.text and "like tea" not in caplog.text


def test_unwritable_queue_is_swallowed(tmp_path, caplog) -> None:
    ro = tmp_path / "ro"
    ro.mkdir()
    ro.chmod(0o500)
    try:
        q = CandidateQueue(ro / "q.jsonl")
        got = q.consider(
            node_id=NODE_ID, room=ROOM, user_text="remember that I like tea", vesper_reply="Ok."
        )
    finally:
        ro.chmod(0o700)
    assert got is None
    assert "memory candidate not staged" in caplog.text and "like tea" not in caplog.text


def test_concurrent_appends_never_interleave(tmp_path) -> None:
    path = tmp_path / "q.jsonl"
    queues = [CandidateQueue(path) for _ in range(4)]  # separate locks: flock must serialise
    long_text = "remember that " + "I like very long coffee orders " * 8

    def worker(q: CandidateQueue) -> None:
        for _ in range(25):
            q.consider(node_id=NODE_ID, room=ROOM, user_text=long_text, vesper_reply="ok " * 90)

    threads = [threading.Thread(target=worker, args=(q,)) for q in queues]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    lines = _lines(path)  # every line parses
    assert len(lines) == 100 and len({line["id"] for line in lines}) == 100


def test_rotation_by_vesper_is_followed(tmp_path) -> None:
    """Vesper archives the queue by renaming it; the next append starts a new file."""
    path = tmp_path / "q.jsonl"
    q = CandidateQueue(path)
    q.append(_candidate())
    path.rename(tmp_path / "archive.jsonl")
    q.append(_candidate(text="I'm allergic to peanuts"))
    assert len(_lines(path)) == 1 and len(_lines(tmp_path / "archive.jsonl")) == 1
    assert stat.S_IMODE(path.stat().st_mode) == 0o600


def test_large_queue_warns(tmp_path, caplog) -> None:
    q = CandidateQueue(tmp_path / "q.jsonl", warn_at=2)
    for _ in range(3):
        q.consider(
            node_id=NODE_ID, room=ROOM, user_text="remember that I like tea", vesper_reply=""
        )
    assert "memory candidate queue has 3 entries" in caplog.text


def test_non_significant_turn_creates_no_file(tmp_path) -> None:
    path = tmp_path / "memory" / "q.jsonl"
    q = CandidateQueue(path)
    assert q.consider(node_id=NODE_ID, room=ROOM, user_text="hello", vesper_reply="Hi!") is None
    assert not path.parent.exists()


# ---- the turn pipeline ----------------------------------------------------------------------


class Brain:
    def __init__(self) -> None:
        self.reply = "Got it."
        self.status = 200
        self.texts: list[str] = []

    async def __call__(self, request: httpx.Request) -> httpx.Response:
        self.texts.append(json.loads(request.content)["text"])
        if self.status != 200:
            return httpx.Response(self.status, json={})
        return httpx.Response(200, json={"text": self.reply, "actions": []})


class STT:
    def __init__(self) -> None:
        self.queue: list[str] = []

    async def __call__(self, request: httpx.Request) -> httpx.Response:
        return httpx.Response(200, json={"text": self.queue.pop(0)})


@pytest.fixture
def brain() -> Brain:
    return Brain()


@pytest.fixture
def stt() -> STT:
    return STT()


@pytest.fixture
def mkapp(brain, stt, registry, tmp_path):
    def _mk(queue: CandidateQueue | None = None):
        return create_app(
            make_settings(tts_off=True),
            stt_transport=httpx.MockTransport(stt),
            brain_transport=httpx.MockTransport(brain),
            registry=registry,
            conversations=ConversationStore(None),
            candidates=queue or CandidateQueue(tmp_path / "memory" / "node-candidates.jsonl"),
        )

    return _mk


async def say(app, stt: STT, text: str) -> dict[str, Any]:
    stt.queue.append(text)
    async with client_for(app) as c:
        r = await c.post("/turn", content=make_note(), headers=turn_headers())
    return dict(parse_sse(r.text))


async def settle(app) -> None:
    while pending := [t for t in app.app.state.background if t.get_name() == "memory-candidate"]:
        await asyncio.gather(*pending, return_exceptions=True)


async def test_turn_with_remember_stages_one_candidate(mkapp, stt, brain, tmp_path) -> None:
    app = mkapp()
    brain.reply = "Got it, black coffee."
    ev = await say(app, stt, "Remember that I like my coffee black")
    assert ev["done"] == {"ok": True}
    await settle(app)
    (line,) = _lines(tmp_path / "memory" / "node-candidates.jsonl")
    assert line["node_id"] == NODE_ID and line["room"] == ROOM
    assert line["user_text"] == "Remember that I like my coffee black"
    assert line["vesper_reply"] == "Got it, black coffee." and line["status"] == "pending"
    assert line["speaker"] == "unverified"
    assert len(brain.texts) == 1  # the memory path never called the brain


async def test_bare_remember_that_stages_the_previous_exchange(mkapp, stt, brain) -> None:
    queue = CandidateQueue(None)
    app = mkapp(queue)
    brain.reply = "The gate code is 4821."
    await say(app, stt, "What's the gate code")
    brain.reply = "Okay, I'll remember."
    await say(app, stt, "Remember that")
    await settle(app)
    (cand,) = queue.staged
    assert (cand["user_text"], cand["vesper_reply"]) == (
        "What's the gate code",
        "The gate code is 4821.",
    )
    assert len(brain.texts) == 2


async def test_room_assignment_and_failed_turns_never_stage(mkapp, stt, brain) -> None:
    queue = CandidateQueue(None)
    app = mkapp(queue)
    await say(app, stt, "You're in the office")  # roomcmd: no candidate
    brain.status = 500
    ev = await say(app, stt, "Remember that I like my coffee black")  # ask failed
    assert ev["done"] == {"ok": False}
    brain.status = 200
    await say(app, stt, "Turn off the kitchen lights")
    await say(app, stt, "What's the weather")
    await settle(app)
    assert queue.staged == []


class SlowQueue(CandidateQueue):
    def __init__(self, delay: float) -> None:
        super().__init__(None)
        self.delay = delay
        self.started = threading.Event()

    def consider(self, **kw: Any) -> dict[str, Any] | None:
        self.started.set()
        time.sleep(self.delay)  # blocking on purpose: must run off the event loop
        return super().consider(**kw)


async def test_slow_stager_adds_no_latency(mkapp, stt) -> None:
    queue = SlowQueue(1.0)
    app = mkapp(queue)
    t0 = time.monotonic()
    ev = await say(app, stt, "Remember that I like my coffee black")
    elapsed = time.monotonic() - t0
    assert ev["done"] == {"ok": True}
    assert elapsed < 0.5, elapsed  # the turn did not wait for the 1 s stager
    assert queue.staged == []  # not staged yet: it runs after the turn
    t1 = time.monotonic()
    ev = await say(app, stt, "hello")  # the event loop is not blocked by the stager either
    assert time.monotonic() - t1 < 0.5 and ev["done"] == {"ok": True}
    await settle(app)
    assert len(queue.staged) == 1


class BrokenQueue(CandidateQueue):
    def consider(self, **kw: Any) -> dict[str, Any] | None:
        raise RuntimeError("boom: remember that I like my coffee black")


async def test_raising_stager_does_not_fail_the_turn(mkapp, stt, caplog) -> None:
    caplog.set_level(logging.INFO)
    app = mkapp(BrokenQueue(None))
    ev = await say(app, stt, "Remember that I like my coffee black")
    assert ev["done"] == {"ok": True} and ev["text_delta"]["text"] == "Got it."
    await settle(app)
    assert "memory candidate failed" in caplog.text and "RuntimeError" in caplog.text
    assert "coffee" not in caplog.text


async def test_candidate_text_never_logged(mkapp, stt, brain, caplog) -> None:
    caplog.set_level(logging.DEBUG)
    app = mkapp()
    brain.reply = "Noted, black coffee."
    await say(app, stt, "Remember that I like my coffee black")
    await say(app, stt, "I'm allergic to peanuts")
    await settle(app)
    text = caplog.text + "".join(str(r.args) for r in caplog.records)
    assert "memory candidate staged" in text
    for needle in ("coffee", "peanuts", "black", "Noted"):
        assert needle not in text, needle


# ---- safety: the memory path can never act --------------------------------------------------

ALLOWED_IMPORTS = {
    "__future__",
    "collections.abc",
    "dataclasses",
    "datetime",
    "errno",
    "fcntl",
    "hashlib",
    "json",
    "logging",
    "os",
    "pathlib",
    "re",
    "secrets",
    "stat",
    "threading",
    "typing",
}


def _code_tokens(path: Path) -> set[str]:
    """Identifiers, attribute names and non-docstring string constants (no comments/docs)."""
    tree = ast.parse(path.read_text())
    docstrings = {
        id(n.body[0].value)
        for n in ast.walk(tree)
        if isinstance(n, ast.Module | ast.ClassDef | ast.FunctionDef | ast.AsyncFunctionDef)
        and n.body
        and isinstance(n.body[0], ast.Expr)
        and isinstance(n.body[0].value, ast.Constant)
    }
    out: set[str] = set()
    for n in ast.walk(tree):
        if isinstance(n, ast.Name):
            out.add(n.id)
        elif isinstance(n, ast.Attribute):
            out.add(n.attr)
        elif isinstance(n, ast.Constant) and isinstance(n.value, str) and id(n) not in docstrings:
            out.add(n.value)
    return out


def test_candidates_module_imports_only_the_stdlib_allowlist() -> None:
    tree = ast.parse(Path(cand_mod.__file__).read_text())
    imported: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            imported.update(a.name for a in node.names)
        elif isinstance(node, ast.ImportFrom):
            assert node.level == 0, "no relative (vesper_node) imports in the memory module"
            imported.add(node.module or "")
    assert imported <= ALLOWED_IMPORTS, imported - ALLOWED_IMPORTS
    tokens = _code_tokens(Path(cand_mod.__file__))
    for forbidden in ("httpx", "BrainClient", "ask", "ask_url", "post", "subprocess", "system"):
        assert forbidden not in tokens, forbidden
    assert not any("/ask" in t or "lobe" in t.lower() for t in tokens)


def test_importing_candidates_loads_no_action_capable_module() -> None:
    code = (
        "import sys, vesper_node.candidates; "
        "bad = [m for m in ('httpx', 'vesper_node.brain', 'vesper_node.app', "
        "'vesper_node.registry', 'vesper_node.conversation', 'vesper_node.tts', "
        "'vesper_node.stt') if m in sys.modules]; print(bad); sys.exit(1 if bad else 0)"
    )
    r = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, check=False)  # noqa: S603
    assert r.returncode == 0, r.stdout + r.stderr


def test_no_node_code_reads_curated_memory() -> None:
    """Only the candidate queue path (config default) and docs mention ~/memory/MEMORY.md."""
    for path in SRC.glob("*.py"):
        mentions = [t for t in _code_tokens(path) if "MEMORY.md" in t or "memory/" in t]
        if path.name == "config.py":  # only DEFAULT_CANDIDATES_FILE
            assert mentions == ["~/memory/node-candidates.jsonl"], mentions
        else:
            assert mentions == [], (path.name, mentions)


def test_queue_opens_the_file_append_only() -> None:
    source = Path(cand_mod.__file__).read_text()
    assert "O_APPEND" in source and "O_TRUNC" not in source and "O_RDWR" not in source
    assert "os.replace" not in source and "write_text" not in source


def test_config_default_candidates_path(monkeypatch, tmp_path) -> None:
    from vesper_node.config import DEFAULT_CANDIDATES_FILE, load_settings

    base = {
        "VESPER_NODE_TOKEN": "n" * 40,
        "VESPER_BRAIN_TOKEN": "b" * 40,
        "ELEVENLABS_API_KEY": "sk_" + "e" * 48,
    }
    none = tmp_path / "none.env"
    s = load_settings(base, node_env_file=none, voice_env_file=none)
    assert s.candidates_file == str(DEFAULT_CANDIDATES_FILE.expanduser())
    assert s.candidates_file.endswith("/memory/node-candidates.jsonl")
    s = load_settings(
        {**base, "VESPER_NODE_CANDIDATES_FILE": str(tmp_path / "c.jsonl")},
        node_env_file=none,
        voice_env_file=none,
    )
    assert s.candidates_file == str(tmp_path / "c.jsonl")
