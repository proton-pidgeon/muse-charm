"""Per-node conversation memory (task 16, Parts 1 + 2): working transcript + session seed.

Each node keeps its last :data:`MAX_TURNS` exchanges (Kevin's words + Vesper's reply) and an
optional **session summary**. On every ``/turn`` the history is prepended to the brain
``/ask`` text by :func:`build_ask_text`, so "tell me more about that" sees the previous reply.

Session awareness: when a turn arrives more than ``idle_s`` (default 15 min) after the node's
previous turn, the old transcript is compressed into a short **extractive** summary
(:func:`extractive_summary`: Kevin's last questions + the start of Vesper's last reply, no
LLM call), stored as the session seed, and the transcript is cleared. The brain is never asked
to summarize: its ``/ask`` always runs the home-control tool loop, so replaying old
utterances ("turn off the kitchen lights") to it could act on them. The summary is prepended
like the transcript, so "what were we talking about" still works across the gap.

Storage: one JSON file (default ``~/.config/vesper-voice/node-memory.json``, override
``VESPER_NODE_MEMORY_FILE``) on the Studio only. It is written like the registry: mode 600 in
a 700 directory, temp file + ``fsync`` + ``os.replace`` + directory ``fsync``, so a crash
leaves the old or the new file, never a torn one. A file that is not a private regular file,
or is malformed, is ignored (logged by name, never by content) and replaced on the next write.
Only the running service writes it, so the in-memory copy is authoritative after the first
load. Timestamps are wall-clock (``time.time``) so idle detection survives a restart.

Privacy: transcript text is Kevin's own voice. It is never logged. Logs carry turn counts,
char lengths and status labels only.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import logging
import os
import re
import stat
import tempfile
import threading
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .brain import with_room_context

log = logging.getLogger("vesper_node.conversation")

SCHEMA_VERSION = 1
MAX_TURNS = 15
SESSION_IDLE_S = 15 * 60
# Hard ceiling on the whole history block in the /ask text (the /ask limit is 1000 chars;
# the room prefix and Kevin's own words always come first, history gets what is left).
HISTORY_BUDGET = 640
VERBATIM_TURNS = 3  # the newest turns keep (nearly) their full text
VERBATIM_SIDE_CHARS = 320  # per side, newest turns (TTS replies are capped at 500 anyway)
OLDER_SIDE_CHARS = 60  # per side, older turns
SUMMARY_MAX_CHARS = 300
STORED_SIDE_CHARS = 1000  # bound what is persisted per side (STT/brain outputs are bounded)
MIN_TURN_CHARS = 40  # do not add a history turn squeezed below this

_WS = re.compile(r"\s+")


def _squash(text: str) -> str:
    return _WS.sub(" ", text).strip()


def shorten(text: str, limit: int) -> str:
    """Collapse whitespace; if still over ``limit``, cut at a word boundary and add ``…``."""
    text = _squash(text)
    if len(text) <= limit:
        return text
    if limit <= 1:
        return ""
    cut = text[: limit - 1]
    if " " in cut[limit // 2 :]:
        cut = cut[: cut.rfind(" ")]
    return cut.rstrip(" ,;:.-") + "…"


@dataclass
class Turn:
    user: str
    reply: str
    ts: float


@dataclass
class NodeMemory:
    turns: list[Turn] = field(default_factory=list)
    summary: str | None = None
    last_ts: float | None = None

    def copy(self) -> NodeMemory:
        return NodeMemory(list(self.turns), self.summary, self.last_ts)


# ---- prompt construction ------------------------------------------------------------------


def _render_turn(turn: Turn, side_chars: int) -> str:
    return f"Kevin: {shorten(turn.user, side_chars)} / Vesper: {shorten(turn.reply, side_chars)}"


def history_block(memory: NodeMemory, budget: int) -> str:
    """The compact history prefix, at most ``budget`` chars (``""`` if nothing fits).

    Newest turns win: they are added newest-first, the newest :data:`VERBATIM_TURNS` with
    up to :data:`VERBATIM_SIDE_CHARS` per side and older ones truncated harder, until the
    budget runs out. Then the session summary, if there is room. Rendered oldest-first.
    """
    if budget <= 0 or (not memory.turns and not memory.summary):
        return ""
    head, sep, tail = "[Recent conversation, oldest first: ", " // ", "] "
    summary_head, summary_tail = "[Earlier session summary: ", "] "
    chosen: list[str] = []
    used = len(head) + len(tail)
    for i, turn in enumerate(reversed(memory.turns)):
        side = VERBATIM_SIDE_CHARS if i < VERBATIM_TURNS else OLDER_SIDE_CHARS
        rendered = _render_turn(turn, side)
        extra = len(rendered) + (len(sep) if chosen else 0)
        if used + extra > budget:
            room = budget - used - (len(sep) if chosen else 0)
            if room >= MIN_TURN_CHARS:
                # squeeze this one turn into what is left, then stop
                per_side = max(8, (room - len("Kevin:  / Vesper: ")) // 2)
                rendered = _render_turn(turn, per_side)
                while len(rendered) > room and per_side > 8:
                    per_side -= 4
                    rendered = _render_turn(turn, per_side)
                if len(rendered) <= room:
                    chosen.append(rendered)
            break
        chosen.append(rendered)
        used += extra
    turns_text = head + sep.join(reversed(chosen)) + tail if chosen else ""
    summary_text = ""
    if memory.summary:
        room = budget - len(turns_text) - len(summary_head) - len(summary_tail)
        if room >= MIN_TURN_CHARS:
            summary_text = summary_head + shorten(memory.summary, room) + summary_tail
    return summary_text + turns_text


def build_ask_text(
    text: str, room: str | None, memory: NodeMemory | None, *, limit: int, budget: int
) -> str:
    """``[room] [summary] [history] text``, never over ``limit``. Kevin's words are never cut.

    If ``text`` plus the room prefix alone is over ``limit`` the result is over ``limit``
    too, and the caller refuses the turn (the existing refuse-not-clip rule).
    """
    bare = with_room_context(text, room)
    if memory is None:
        return bare
    room_for_history = min(budget, limit - len(bare))
    block = history_block(memory, room_for_history)
    return with_room_context(block + text, room) if block else bare


def extractive_summary(memory: NodeMemory) -> str | None:
    """Session summary without an LLM: Kevin's last three questions + the start of Vesper's
    last reply, always ``<= SUMMARY_MAX_CHARS``. An empty transcript keeps the old summary."""
    if not memory.turns:
        return shorten(memory.summary, SUMMARY_MAX_CHARS) if memory.summary else None
    asked = "; ".join(shorten(t.user, 70).rstrip(".!?") for t in memory.turns[-3:])
    last = memory.turns[-1].reply
    first_sentence = re.split(r"(?<=[.!?])\s", _squash(last), maxsplit=1)[0]
    text = f"Kevin asked: {asked}. Vesper last said: {shorten(first_sentence, 120)}"
    return shorten(text, SUMMARY_MAX_CHARS)


# ---- storage ------------------------------------------------------------------------------


def _parse(data: Any) -> dict[str, NodeMemory]:
    if not isinstance(data, dict) or data.get("version") != SCHEMA_VERSION:
        raise ValueError("schema")
    nodes = data.get("nodes")
    if not isinstance(nodes, dict):
        raise ValueError("schema")
    out: dict[str, NodeMemory] = {}
    for node_id, rec in nodes.items():
        if not isinstance(node_id, str) or not isinstance(rec, dict):
            raise ValueError("node")
        turns = []
        for t in rec.get("turns") or []:
            if not (
                isinstance(t, dict)
                and isinstance(t.get("user"), str)
                and isinstance(t.get("reply"), str)
                and isinstance(t.get("ts"), int | float)
            ):
                raise ValueError("turn")
            turns.append(Turn(t["user"], t["reply"], float(t["ts"])))
        summary = rec.get("summary")
        last_ts = rec.get("last_ts")
        if summary is not None and not isinstance(summary, str):
            raise ValueError("summary")
        if last_ts is not None and not isinstance(last_ts, int | float):
            raise ValueError("last_ts")
        out[node_id] = NodeMemory(
            turns[-MAX_TURNS:], summary, None if last_ts is None else float(last_ts)
        )
    return out


def _dump(nodes: dict[str, NodeMemory]) -> bytes:
    data = {
        "version": SCHEMA_VERSION,
        "nodes": {
            nid: {
                "turns": [{"user": t.user, "reply": t.reply, "ts": t.ts} for t in m.turns],
                "summary": m.summary,
                "last_ts": m.last_ts,
            }
            for nid, m in sorted(nodes.items())
        },
    }
    return json.dumps(data, indent=1, ensure_ascii=False).encode() + b"\n"


class ConversationStore:
    """Per-node transcripts + session seeds. ``path=None`` keeps everything in RAM (tests)."""

    def __init__(
        self,
        path: Path | str | None,
        *,
        max_turns: int = MAX_TURNS,
        idle_s: float = SESSION_IDLE_S,
        clock: Callable[[], float] = time.time,
    ) -> None:
        self.path = Path(path).expanduser() if path is not None else None
        self.max_turns = max_turns
        self.idle_s = idle_s
        self.clock = clock
        self._nodes: dict[str, NodeMemory] | None = None
        self._io = threading.Lock()  # file + cache (to_thread workers)
        self._locks: dict[str, asyncio.Lock] = {}

    def lock(self, node_id: str) -> asyncio.Lock:
        """The per-node lock: one turn at a time reads-and-updates a node's memory."""
        lk = self._locks.get(node_id)
        if lk is None:
            lk = self._locks[node_id] = asyncio.Lock()
        return lk

    # -- file I/O (blocking: call through asyncio.to_thread from the event loop) --

    def _load_file(self) -> dict[str, NodeMemory]:
        if self.path is None:
            return {}
        try:
            fd = os.open(self.path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
        except FileNotFoundError:
            return {}
        except OSError as e:
            log.error("node memory unreadable: file=%s reason=%s", self.path, type(e).__name__)
            return {}
        with os.fdopen(fd, "rb") as fh:
            st = os.fstat(fh.fileno())
            if not stat.S_ISREG(st.st_mode) or stat.S_IMODE(st.st_mode) & 0o077:
                log.error("node memory ignored: file=%s must be a mode-600 file", self.path)
                return {}
            raw = fh.read()
        try:
            nodes = _parse(json.loads(raw.decode("utf-8")))
        except (ValueError, UnicodeDecodeError):
            log.error("node memory ignored: file=%s is malformed", self.path)
            return {}
        log.info("node memory loaded: nodes=%d bytes=%d", len(nodes), len(raw))
        return nodes

    def _write_file(self, payload: bytes) -> None:
        assert self.path is not None
        parent = self.path.parent
        if not parent.exists():
            parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=parent, prefix=f".{self.path.name}.")
        try:
            os.fchmod(fd, 0o600)
            with os.fdopen(fd, "wb") as fh:
                fh.write(payload)
                fh.flush()
                os.fsync(fh.fileno())
            os.replace(tmp, self.path)
        except BaseException:
            with contextlib.suppress(FileNotFoundError):
                os.unlink(tmp)
            raise
        dfd = os.open(parent, os.O_RDONLY)
        try:
            os.fsync(dfd)
        finally:
            os.close(dfd)

    def _cache(self) -> dict[str, NodeMemory]:
        if self._nodes is None:
            self._nodes = self._load_file()
        return self._nodes

    def get_sync(self, node_id: str) -> NodeMemory:
        with self._io:
            return self._cache().get(node_id, NodeMemory()).copy()

    def put_sync(self, node_id: str, memory: NodeMemory) -> None:
        """Replace one node's memory and persist it.

        On a write error (``OSError``) the new state stays in RAM, so the conversation keeps
        working until a restart, and the error is raised for the caller to log.
        """
        with self._io:
            nodes = self._cache()
            nodes[node_id] = memory.copy()
            if self.path is not None:
                self._write_file(_dump(nodes))

    async def get(self, node_id: str) -> NodeMemory:
        return await asyncio.to_thread(self.get_sync, node_id)

    async def put(self, node_id: str, memory: NodeMemory) -> None:
        await asyncio.to_thread(self.put_sync, node_id, memory)

    # -- operations (call with ``lock(node_id)`` held) --

    def is_idle(self, memory: NodeMemory, now: float) -> bool:
        return (
            bool(memory.turns)
            and memory.last_ts is not None
            and now - memory.last_ts > (self.idle_s)
        )

    def append(self, memory: NodeMemory, user: str, reply: str, now: float) -> NodeMemory:
        turns = [*memory.turns, Turn(user[:STORED_SIDE_CHARS], reply[:STORED_SIDE_CHARS], now)]
        return NodeMemory(turns[-self.max_turns :], memory.summary, now)

    @staticmethod
    def compressed(memory: NodeMemory, summary: str | None) -> NodeMemory:
        """The session seed: transcript cleared, summary kept (``last_ts`` unchanged)."""
        return NodeMemory([], summary or memory.summary, memory.last_ts)
