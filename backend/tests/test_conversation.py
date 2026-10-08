"""Task 16 Parts 1+2: per-node working transcript, persistence, session summaries."""

from __future__ import annotations

import asyncio
import json
import logging
import os
import stat
from typing import Any

import httpx
import pytest
from conftest import (
    NODE_ID,
    ROOM,
    client_for,
    make_note,
    make_settings,
    parse_sse,
    turn_headers,
)

from vesper_node.app import create_app
from vesper_node.brain import MAX_TEXT_CHARS
from vesper_node.conversation import (
    HISTORY_BUDGET,
    MAX_TURNS,
    SUMMARY_INSTRUCTION,
    ConversationStore,
    NodeMemory,
    Turn,
    build_ask_text,
    extractive_summary,
    shorten,
    summary_request,
)

HEADLINE_Q = "What's the top headline this morning"
HEADLINE_A = "Markets rallied after the central bank hinted at a September rate cut."
FOLLOW_UP = "Tell me more about that"
PREFIX = f"[Vesper node in the {ROOM}] "


class Clock:
    def __init__(self, t: float = 1_800_000_000.0) -> None:
        self.t = t

    def __call__(self) -> float:
        return self.t


class Brain:
    """Fake brain: scripted replies, records every /ask text, can fail summaries only."""

    def __init__(self) -> None:
        self.replies: list[str] = []
        self.default = "Okay."
        self.texts: list[str] = []
        self.summary = "Kevin asked about the markets headline and a rate cut."
        self.summary_status = 200
        self.status = 200
        self.summary_gate: asyncio.Event | None = None
        self.gate: asyncio.Event | None = None

    async def __call__(self, request: httpx.Request) -> httpx.Response:
        text = json.loads(request.content)["text"]
        self.texts.append(text)
        if text.startswith(SUMMARY_INSTRUCTION):
            if self.summary_gate is not None:
                await self.summary_gate.wait()
            return httpx.Response(self.summary_status, json={"text": self.summary})
        if self.gate is not None:
            await self.gate.wait()
        if self.status != 200:
            return httpx.Response(self.status, json={})
        reply = self.replies.pop(0) if self.replies else self.default
        return httpx.Response(200, json={"text": reply, "actions": []})

    @property
    def turn_texts(self) -> list[str]:
        return [t for t in self.texts if not t.startswith(SUMMARY_INSTRUCTION)]

    @property
    def summary_texts(self) -> list[str]:
        return [t for t in self.texts if t.startswith(SUMMARY_INSTRUCTION)]


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
def clock() -> Clock:
    return Clock()


@pytest.fixture
def mkapp(brain, stt, registry, clock, tmp_path):
    def _mk(store: ConversationStore | None = None, **kwargs: Any):
        store = store or ConversationStore(tmp_path / "cfg" / "node-memory.json", clock=clock)
        return create_app(
            make_settings(tts_off=True),
            stt_transport=httpx.MockTransport(stt),
            brain_transport=httpx.MockTransport(brain),
            registry=registry,
            conversations=store,
            **kwargs,
        )

    return _mk


async def say(app, stt: STT, text: str) -> dict[str, Any]:
    stt.queue.append(text)
    async with client_for(app) as c:
        r = await c.post("/turn", content=make_note(), headers=turn_headers())
    return dict(parse_sse(r.text))


def store_of(app) -> ConversationStore:
    return app.app.state.conversations


# ---- Part 1: working transcript -----------------------------------------------------------


async def test_follow_up_sees_previous_reply(mkapp, brain, stt) -> None:
    """The bug Kevin hit: "tell me more about that" must see turn 1."""
    app = mkapp()
    brain.replies = [HEADLINE_A, "The cut would be the first this year."]
    assert (await say(app, stt, HEADLINE_Q))["done"] == {"ok": True}
    assert (await say(app, stt, FOLLOW_UP))["done"] == {"ok": True}
    first, second = brain.turn_texts
    assert first == PREFIX + HEADLINE_Q  # no history: the task-08 text, unchanged
    assert second.startswith(PREFIX + "[Recent conversation, oldest first: ")
    assert f"Kevin: {HEADLINE_Q} / Vesper: {HEADLINE_A}" in second
    assert second.endswith("] " + FOLLOW_UP)
    assert len(second) <= MAX_TEXT_CHARS


async def test_transcript_capped_at_15_turns(mkapp, brain, stt) -> None:
    app = mkapp()
    for i in range(MAX_TURNS + 5):
        await say(app, stt, f"question number {i}")
    turns = store_of(app).get_sync(NODE_ID).turns
    assert len(turns) == MAX_TURNS == 15
    assert [t.user for t in turns] == [f"question number {i}" for i in range(5, 20)]
    last = brain.turn_texts[-1]
    assert "question number 18" in last and "question number 4 " not in last
    assert len(last) <= MAX_TEXT_CHARS


async def test_transcript_survives_restart(mkapp, brain, stt, tmp_path, clock) -> None:
    path = tmp_path / "cfg" / "node-memory.json"
    brain.replies = [HEADLINE_A]
    await say(mkapp(ConversationStore(path, clock=clock)), stt, HEADLINE_Q)
    assert stat.S_IMODE(path.stat().st_mode) == 0o600
    assert stat.S_IMODE(path.parent.stat().st_mode) == 0o700
    # a brand-new store + app, as after a backend restart
    app2 = mkapp(ConversationStore(path, clock=clock))
    await say(app2, stt, FOLLOW_UP)
    assert HEADLINE_A in brain.turn_texts[-1]
    assert not [p for p in path.parent.iterdir() if p.name.startswith(".node-memory")]


async def test_failed_ask_is_not_recorded(mkapp, brain, stt) -> None:
    app = mkapp()
    brain.status = 502
    ev = await say(app, stt, HEADLINE_Q)
    assert ev["error"]["code"] == "ask_failed"
    assert store_of(app).get_sync(NODE_ID).turns == []
    brain.status = 200
    await say(app, stt, FOLLOW_UP)
    assert brain.turn_texts[-1] == PREFIX + FOLLOW_UP


async def test_too_long_utterance_still_refused_not_clipped(mkapp, brain, stt) -> None:
    app = mkapp()
    await say(app, stt, HEADLINE_Q)
    ev = await say(app, stt, "word " * 300)
    assert ev["error"]["code"] == "transcript_too_long"
    assert len(brain.turn_texts) == 1
    assert len(store_of(app).get_sync(NODE_ID).turns) == 1


async def test_history_shrinks_for_a_long_utterance(mkapp, brain, stt) -> None:
    """Near the limit, history gives way; Kevin's words always arrive whole."""
    app = mkapp()
    brain.default = "r" * 480
    for i in range(5):
        await say(app, stt, f"long reply please {i}")
    long_q = "w" * (MAX_TEXT_CHARS - len(PREFIX) - 30)
    ev = await say(app, stt, long_q)
    assert ev["done"] == {"ok": True}
    sent = brain.turn_texts[-1]
    assert sent.endswith(long_q) and len(sent) <= MAX_TEXT_CHARS


async def test_overlapping_turns_for_one_node_both_recorded(mkapp, brain, stt, tmp_path) -> None:
    app = mkapp()
    brain.gate = asyncio.Event()
    stt.queue += ["first question", "second question"]
    async with client_for(app) as c:
        t1 = asyncio.create_task(c.post("/turn", content=make_note(), headers=turn_headers()))
        t2 = asyncio.create_task(c.post("/turn", content=make_note(), headers=turn_headers()))
        for _ in range(200):
            await asyncio.sleep(0.01)
            if len(brain.texts) == 2:
                break
        brain.gate.set()
        r1, r2 = await t1, await t2
    assert dict(parse_sse(r1.text))["done"] == dict(parse_sse(r2.text))["done"] == {"ok": True}
    assert sorted(t.user for t in store_of(app).get_sync(NODE_ID).turns) == [
        "first question",
        "second question",
    ]
    reloaded = ConversationStore(store_of(app).path).get_sync(NODE_ID)
    assert len(reloaded.turns) == 2


async def test_nodes_do_not_share_history(clock, tmp_path) -> None:
    store = ConversationStore(tmp_path / "m.json", clock=clock)
    store.put_sync("node-a", store.append(NodeMemory(), "secret a", "reply a", clock()))
    assert store.get_sync("node-b").turns == []
    assert ConversationStore(tmp_path / "m.json").get_sync("node-a").turns[0].user == "secret a"


@pytest.mark.parametrize("utterance_len", [0, 1, 50, 200, 400, 600, 800, 900, 950, 960, 971])
@pytest.mark.parametrize("summary", [None, "s" * 300])
def test_budget_never_exceeded(utterance_len: int, summary: str | None) -> None:
    assert len(PREFIX) + 971 == MAX_TEXT_CHARS  # 971: the longest utterance that is accepted
    turns = [Turn("q" * 1000, "a" * 1000, float(i)) for i in range(MAX_TURNS)]
    memory = NodeMemory(turns, summary, 1.0)
    text = "u" * utterance_len
    out = build_ask_text(text, ROOM, memory, limit=MAX_TEXT_CHARS, budget=HISTORY_BUDGET)
    assert len(out) <= MAX_TEXT_CHARS
    assert out.endswith(text) and out.startswith(PREFIX)
    history = out[len(PREFIX) : len(out) - len(text)]
    assert len(history) <= HISTORY_BUDGET


def test_recent_turns_verbatim_older_truncated() -> None:
    turns = [Turn(f"question {i} " + "x" * 100, f"answer {i}", float(i)) for i in range(6)]
    out = build_ask_text("next", None, NodeMemory(turns), limit=1000, budget=HISTORY_BUDGET)
    assert "question 5 " + "x" * 100 in out  # newest: verbatim
    assert "question 0 " + "x" * 100 not in out  # oldest: truncated (or dropped)
    assert out.index("question 3") < out.index("question 5")  # rendered oldest first


def test_shorten_cuts_at_word_boundary() -> None:
    assert shorten("one  two\nthree", 50) == "one two three"
    cut = shorten("alpha beta gamma delta epsilon", 16)
    assert len(cut) <= 16 and cut.endswith("…") and cut.startswith("alpha beta")


@pytest.mark.parametrize(
    "content",
    ['{"version": 99}', "not json", '{"version": 1, "nodes": {"n": {"turns": [{}]}}}'],
)
def test_malformed_memory_file_is_ignored(tmp_path, caplog, content: str) -> None:
    path = tmp_path / "m.json"
    path.write_text(content)
    path.chmod(0o600)
    caplog.set_level(logging.INFO)
    assert ConversationStore(path).get_sync(NODE_ID).turns == []
    assert "malformed" in caplog.text


def test_group_readable_memory_file_is_not_read(tmp_path, clock) -> None:
    path = tmp_path / "m.json"
    ConversationStore(path, clock=clock).put_sync(
        NODE_ID, NodeMemory([Turn("private words", "reply", 1.0)], None, 1.0)
    )
    os.chmod(path, 0o644)
    assert ConversationStore(path).get_sync(NODE_ID).turns == []


# ---- Part 2: session awareness ------------------------------------------------------------


async def test_idle_session_compressed_to_summary_and_seeds_next_turns(
    mkapp, brain, stt, clock
) -> None:
    app = mkapp()
    brain.replies = [HEADLINE_A]
    await say(app, stt, HEADLINE_Q)
    clock.t += 10 * 60  # 10 min: still the same session
    await say(app, stt, FOLLOW_UP)
    assert brain.summary_texts == []
    clock.t += 15 * 60 + 1  # idle > 15 min
    ev = await say(app, stt, "What were we talking about")
    assert ev["done"] == {"ok": True}
    (req,) = brain.summary_texts
    assert len(req) <= MAX_TEXT_CHARS and HEADLINE_A in req and HEADLINE_Q in req
    seeded = brain.turn_texts[-1]
    assert f"[Earlier session summary: {brain.summary}]" in seeded
    assert "Recent conversation" not in seeded  # the old transcript was cleared
    memory = store_of(app).get_sync(NODE_ID)
    assert memory.summary == brain.summary
    assert [t.user for t in memory.turns] == ["What were we talking about"]
    # the summary keeps seeding the following turns, next to the new transcript
    await say(app, stt, "And the weather")
    later = brain.turn_texts[-1]
    assert brain.summary in later and "What were we talking about" in later
    assert len(brain.summary_texts) == 1


async def test_summary_survives_restart(mkapp, brain, stt, clock, tmp_path) -> None:
    path = tmp_path / "cfg" / "node-memory.json"
    await say(mkapp(ConversationStore(path, clock=clock)), stt, HEADLINE_Q)
    clock.t += 3600
    app2 = mkapp(ConversationStore(path, clock=clock))  # restarted during the gap
    await say(app2, stt, "What were we talking about")
    assert brain.summary in brain.turn_texts[-1]
    assert ConversationStore(path).get_sync(NODE_ID).summary == brain.summary


async def test_extractive_fallback_when_summary_ask_fails(mkapp, brain, stt, clock) -> None:
    app = mkapp()
    brain.replies = [HEADLINE_A]
    await say(app, stt, HEADLINE_Q)
    clock.t += 16 * 60
    brain.summary_status = 500
    ev = await say(app, stt, "What were we talking about")
    assert ev["done"] == {"ok": True}  # the turn itself is unaffected
    summary = store_of(app).get_sync(NODE_ID).summary
    assert summary is not None and HEADLINE_Q in summary
    assert summary in brain.turn_texts[-1]


async def test_extractive_fallback_on_summary_timeout(mkapp, brain, stt, clock) -> None:
    app = mkapp(summary_timeout_s=0.05)
    await say(app, stt, HEADLINE_Q)
    clock.t += 16 * 60
    brain.summary_gate = asyncio.Event()  # never set: the summary ask hangs
    ev = await say(app, stt, "What were we talking about")
    assert ev["done"] == {"ok": True}
    assert HEADLINE_Q in (store_of(app).get_sync(NODE_ID).summary or "")


def test_summary_request_stays_under_limit() -> None:
    turns = [Turn("q" * 1000, "a" * 1000, float(i)) for i in range(MAX_TURNS)]
    req = summary_request(NodeMemory(turns, "old summary " * 30), limit=MAX_TEXT_CHARS)
    assert req is not None and len(req) <= MAX_TEXT_CHARS
    assert summary_request(NodeMemory(), limit=MAX_TEXT_CHARS) is None


def test_extractive_summary_shape() -> None:
    memory = NodeMemory(
        [Turn("first q", "x", 1.0), Turn("second q", "It is sunny. Highs of 70.", 2.0)]
    )
    s = extractive_summary(memory)
    assert s == "Kevin asked: first q; second q. Vesper last said: It is sunny."
    assert extractive_summary(NodeMemory([], "keep me", 1.0)) == "keep me"


# ---- privacy ------------------------------------------------------------------------------


async def test_memory_text_never_logged(mkapp, brain, stt, clock, caplog) -> None:
    caplog.set_level(logging.DEBUG)
    app = mkapp()
    brain.replies = [HEADLINE_A]
    await say(app, stt, HEADLINE_Q)
    await say(app, stt, FOLLOW_UP)
    clock.t += 3600
    await say(app, stt, "What were we talking about")  # brain summary
    clock.t += 3600
    brain.summary_status = 500
    await say(app, stt, "Anything else")  # extractive summary
    await say(app, stt, "You're in the study")  # room assignment
    text = caplog.text + "".join(str(r.args) for r in caplog.records)
    assert "session compressed" in text and "summary=extractive" in text
    for needle in (
        HEADLINE_Q,
        HEADLINE_A,
        "Tell me more",
        "talking about",
        "Anything else",
        "You're in the study",
        brain.summary,
        "Kevin asked",
        "rate cut",
    ):
        assert needle not in text, needle
