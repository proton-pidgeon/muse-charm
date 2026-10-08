"""GET /announcements (task 18): brain claim -> TTS -> node, against a mocked brain."""

from __future__ import annotations

import asyncio
import json
import logging
import time
from typing import Any

import httpx
import pytest
from conftest import (
    BRAIN_TOKEN,
    FAKE_MP3,
    NODE_ID,
    NODE_TOKEN,
    Providers,
    auth,
    bearer,
    client_for,
    make_settings,
)

import vesper_node.app as app_module
from vesper_node.app import create_app
from vesper_node.brain import claim_url_for, parse_announcements

TIMER = "Your ten minute timer is done."
REMINDER = "Reminder: call the plumber."
CLAIM_URL = "http://127.0.0.1:8790/node/announcements/claim"


class Brain:
    """A fake brain: ``/ask`` (unused here) and the announcements claim."""

    def __init__(self) -> None:
        self.status = 200
        self.body: Any = {
            "announcements": [
                {"id": "a1", "kind": "timer", "text": TIMER},
                {"id": "a2", "kind": "reminder", "text": REMINDER},
            ]
        }
        self.raw: bytes | None = None
        self.exc: Exception | None = None
        self.headers: dict[str, str] = {}
        self.claims: list[httpx.Request] = []
        self.asks: list[httpx.Request] = []

    async def __call__(self, request: httpx.Request) -> httpx.Response:
        if request.url.path == "/ask":
            self.asks.append(request)
            return httpx.Response(200, json={"text": "unused", "actions": []})
        self.claims.append(request)
        if self.exc is not None:
            raise self.exc
        if self.raw is not None:
            return httpx.Response(self.status, content=self.raw, headers=self.headers)
        return httpx.Response(self.status, json=self.body, headers=self.headers)


class Clock:
    def __init__(self) -> None:
        self.t = 1000.0

    def __call__(self) -> float:
        return self.t


@pytest.fixture
def brain() -> Brain:
    return Brain()


@pytest.fixture
def clock() -> Clock:
    return Clock()


@pytest.fixture
def mkapp(brain: Brain, clock: Clock, providers: Providers, registry):
    def _mk(**overrides: Any):
        return create_app(
            make_settings(**overrides),
            stt_transport=httpx.MockTransport(providers.stt),
            brain_transport=httpx.MockTransport(brain),
            tts_transport=httpx.MockTransport(providers.tts),
            clock=clock,
            registry=registry,
        )

    return _mk


async def poll(app, headers: dict[str, str] | None = None) -> httpx.Response:
    async with client_for(app) as c:
        return await c.get("/announcements", headers=auth() if headers is None else headers)


async def test_announcements_spoken_and_fetchable(mkapp, brain, providers) -> None:
    app = mkapp()
    r = await poll(app)
    assert r.status_code == 200
    assert r.headers["cache-control"] == "no-store"
    assert r.headers["x-vesper-node-protocol"] == "1"
    items = r.json()["announcements"]
    assert [i["text"] for i in items] == [TIMER, REMINDER]
    assert set(items[0]) == {"text", "audio_url"}  # no brain ids/kinds leak to the node
    for item in items:
        url = item["audio_url"]
        assert url.startswith("audio/") and url.endswith(".mp3")
    assert [json.loads(t.content)["text"] for t in providers.calls["tts"]] == [TIMER, REMINDER]
    async with client_for(app) as c:
        audio = await c.get("/" + items[0]["audio_url"], headers=auth())
    assert audio.status_code == 200 and audio.content == FAKE_MP3


async def test_claim_request_contract(mkapp, brain) -> None:
    await poll(mkapp())
    (req,) = brain.claims
    assert req.method == "POST"
    assert str(req.url) == CLAIM_URL
    assert req.headers["authorization"] == f"Bearer {BRAIN_TOKEN}"
    assert json.loads(req.content) == {"device_id": NODE_ID}
    assert NODE_TOKEN not in str(req.headers) and NODE_TOKEN.encode() not in req.content
    assert brain.asks == []


async def test_claim_url_keeps_a_brain_path_prefix(mkapp, brain) -> None:
    await poll(mkapp(brain_url="http://127.0.0.1:8790/brain"))
    assert str(brain.claims[0].url) == "http://127.0.0.1:8790/brain/node/announcements/claim"


@pytest.mark.parametrize(
    ("ask", "claim"),
    [
        ("http://127.0.0.1:8790/ask", CLAIM_URL),
        (
            "https://brain.example.ts.net/ask",
            "https://brain.example.ts.net/node/announcements/claim",
        ),
        ("http://[::1]:8790/v/ask", "http://[::1]:8790/v/node/announcements/claim"),
    ],
)
def test_claim_url_for(ask: str, claim: str) -> None:
    assert claim_url_for(ask) == claim


async def test_empty_claim_is_empty_list(mkapp, brain, providers) -> None:
    brain.body = {"announcements": []}
    r = await poll(mkapp())
    assert (r.status_code, r.json()) == (200, {"announcements": []})
    assert providers.calls["tts"] == []


async def test_tts_off_means_null_audio(mkapp, brain, providers) -> None:
    r = await poll(mkapp(tts_off=True))
    assert r.json() == {
        "announcements": [{"text": TIMER, "audio_url": None}, {"text": REMINDER, "audio_url": None}]
    }
    assert providers.calls["tts"] == []


class SlowTTS:
    """A TTS stub that takes ``delay_s`` per request and records how many ran at once."""

    def __init__(self, delay_s: float) -> None:
        self.delay_s = delay_s
        self.inflight = 0
        self.max_inflight = 0
        self.calls = 0

    async def __call__(self, request: httpx.Request) -> httpx.Response:
        self.calls += 1
        self.inflight += 1
        self.max_inflight = max(self.max_inflight, self.inflight)
        try:
            await asyncio.sleep(self.delay_s)
        finally:
            self.inflight -= 1
        return httpx.Response(200, content=FAKE_MP3)


def mkapp_with_tts(brain: Brain, clock: Clock, providers: Providers, registry, tts: SlowTTS):
    return create_app(
        make_settings(),
        stt_transport=httpx.MockTransport(providers.stt),
        brain_transport=httpx.MockTransport(brain),
        tts_transport=httpx.MockTransport(tts),
        clock=clock,
        registry=registry,
    )


async def test_tts_over_budget_is_caption_only_within_budget(
    brain, clock, providers, registry, monkeypatch, caplog
) -> None:
    # The brain has already marked the items delivered: a slow TTS must not make the poll
    # outlast the firmware's wait. Over budget -> 200 in time, text present, audio_url null.
    monkeypatch.setattr(app_module, "ANNOUNCE_TTS_BUDGET_S", 0.3)
    caplog.set_level(logging.WARNING)
    tts = SlowTTS(delay_s=5.0)
    app = mkapp_with_tts(brain, clock, providers, registry, tts)
    started = time.monotonic()
    r = await poll(app)
    elapsed = time.monotonic() - started
    assert r.status_code == 200
    assert elapsed < 2.0, elapsed  # the budget, not the TTS delay (5 s) or TTS_TIMEOUT_S (4 s)
    assert r.json() == {
        "announcements": [{"text": TIMER, "audio_url": None}, {"text": REMINDER, "audio_url": None}]
    }
    assert tts.calls == 2  # both were attempted (concurrently), both cut off by the budget
    assert sum("tts over budget" in rec.getMessage() for rec in caplog.records) == 2
    for secret in (TIMER, REMINDER, "plumber"):
        assert secret not in caplog.text


async def test_tts_is_minted_concurrently(brain, clock, providers, registry) -> None:
    # Two items with a 0.4 s TTS each: minted together, so the poll takes ~0.4 s, not ~0.8 s,
    # and both get their audio inside the 2 s budget.
    tts = SlowTTS(delay_s=0.4)
    app = mkapp_with_tts(brain, clock, providers, registry, tts)
    started = time.monotonic()
    r = await poll(app)
    elapsed = time.monotonic() - started
    items = r.json()["announcements"]
    assert [i["text"] for i in items] == [TIMER, REMINDER]
    assert all(i["audio_url"] for i in items)
    assert tts.max_inflight == 2
    assert elapsed < 0.75, elapsed


async def test_tts_failure_means_null_audio(mkapp, brain, providers) -> None:
    providers.tts_status = 500
    r = await poll(mkapp())
    assert r.status_code == 200
    assert [i["audio_url"] for i in r.json()["announcements"]] == [None, None]


@pytest.mark.parametrize(
    "setup",
    [
        pytest.param(lambda b: setattr(b, "status", 500), id="http_500"),
        pytest.param(lambda b: setattr(b, "status", 401), id="http_401"),
        pytest.param(lambda b: setattr(b, "status", 422), id="http_422"),
        pytest.param(lambda b: setattr(b, "exc", httpx.ReadTimeout("t")), id="timeout"),
        pytest.param(lambda b: setattr(b, "exc", httpx.ConnectError("down")), id="down"),
        pytest.param(lambda b: setattr(b, "raw", b"not json"), id="bad_json"),
        pytest.param(lambda b: setattr(b, "body", ["x"]), id="not_object"),
        pytest.param(lambda b: setattr(b, "body", {"announcements": "x"}), id="not_list"),
        pytest.param(lambda b: setattr(b, "body", {}), id="missing"),
    ],
)
async def test_brain_failure_is_an_empty_list_never_5xx(mkapp, brain, providers, setup, caplog):
    setup(brain)
    caplog.set_level(logging.INFO)
    r = await poll(mkapp())
    assert (r.status_code, r.json()) == (200, {"announcements": []})
    assert len(brain.claims) == 1  # no retries: the claim is at most once
    assert providers.calls["tts"] == []
    assert any(r.levelno == logging.WARNING for r in caplog.records)


async def test_redirect_is_not_followed(mkapp, brain) -> None:
    brain.status = 307
    brain.headers = {"Location": "http://evil.example/steal"}
    r = await poll(mkapp())
    assert (r.status_code, r.json()) == (200, {"announcements": []})
    assert len(brain.claims) == 1


def test_strict_validation() -> None:
    ok = {"text": "fine"}
    body = {
        "announcements": [
            ok,
            "a string",
            {"text": 5},
            {"text": ""},
            {"text": "   "},
            {"text": "x" * 201},
            {"kind": "timer"},
            {"text": "x" * 200},
        ]
    }
    texts, dropped, unlabeled = parse_announcements(body)
    assert texts == ["fine"]  # only the first 5 entries are looked at
    assert dropped == 7
    assert unlabeled == 1  # kept, but without the contract's id/kind
    assert parse_announcements({"announcements": [ok] * 9}) == (["fine"] * 5, 4, 5)
    assert parse_announcements({"announcements": [{"text": "a\nb\t c"}]}) == (["a b c"], 0, 1)
    assert parse_announcements(None) == ([], 0, 0)
    labeled = {"id": "a1", "kind": "timer", "text": "fine"}
    assert parse_announcements({"announcements": [labeled]}) == (["fine"], 0, 0)
    for bad in ({**labeled, "kind": "alarm"}, {**labeled, "id": ""}, {**labeled, "id": 3}):
        assert parse_announcements({"announcements": [bad]}) == (["fine"], 0, 1)


async def test_unlabeled_items_are_spoken_and_counted(mkapp, brain, caplog) -> None:
    caplog.set_level(logging.INFO)
    labeled = {"id": "a2", "kind": "reminder", "text": REMINDER}
    brain.body = {"announcements": [{"text": TIMER}, labeled]}
    r = await poll(mkapp())
    assert [i["text"] for i in r.json()["announcements"]] == [TIMER, REMINDER]
    assert any("unlabeled=1" in rec.getMessage() for rec in caplog.records)


async def test_invalid_items_dropped_valid_kept(mkapp, brain) -> None:
    brain.body = {"announcements": [{"text": "x" * 201}, {"text": TIMER}, 7]}
    r = await poll(mkapp())
    (item,) = r.json()["announcements"]
    assert item["text"] == TIMER and item["audio_url"].startswith("audio/")


async def test_polls_faster_than_5s_are_429(mkapp, brain, clock) -> None:
    app = mkapp()
    assert (await poll(app)).status_code == 200
    clock.t += 2.0
    r = await poll(app)
    assert (r.status_code, r.json()) == (429, {"error": "rate_limited"})
    assert r.headers["retry-after"] == "3"
    assert len(brain.claims) == 1  # a hammering node never reaches the brain
    clock.t += 3.0
    assert (await poll(app)).status_code == 200
    assert len(brain.claims) == 2


@pytest.mark.parametrize(
    ("headers", "status"),
    [
        ({}, 401),
        (bearer("wrong-token-" + "x" * 40), 401),
        ({**auth(), "Authorization": f"Bearer {BRAIN_TOKEN}"}, 401),
        (bearer(), 400),  # no X-Node-Id
        ({**bearer(), "X-Node-Id": NODE_ID}, 403),  # no credential
        ({**auth(), "X-Node-Credential": "vnc_" + "x" * 43}, 403),  # wrong credential
        ({**auth(), "X-Node-Id": "homelink-000000000000"}, 403),  # unknown node
    ],
)
async def test_announcements_need_node_auth(mkapp, brain, headers, status) -> None:
    r = await poll(mkapp(), headers)
    assert r.status_code == status
    assert r.json()["error"] in {"unauthorized", "node_unauthorized", "bad_node_id"}
    assert brain.claims == []


async def test_revoked_node_gets_nothing(mkapp, brain, registry) -> None:
    registry.revoke(NODE_ID)
    r = await poll(mkapp())
    assert (r.status_code, r.json()) == (403, {"error": "node_unauthorized"})
    assert brain.claims == []


async def test_head_never_claims(mkapp, brain, providers) -> None:
    # A claim is at most once, so only GET may spend the node's announcements. HEAD (a probe,
    # never the firmware) is 405 on this FastAPI route, never reaching the handler: no claim,
    # no TTS, and the 5 s rate window is untouched. Pinned so a future Starlette-style
    # auto-HEAD can't silently start claiming.
    app = mkapp()
    async with client_for(app) as c:
        r = await c.head("/announcements", headers=auth())
    assert r.status_code == 405
    assert brain.claims == [] and providers.calls["tts"] == []
    assert (await poll(app)).status_code == 200  # the GET right after is not rate limited
    assert len(brain.claims) == 1


async def test_head_without_bearer_is_401(mkapp, brain) -> None:
    async with client_for(mkapp()) as c:
        r = await c.head("/announcements")
    assert r.status_code == 401
    assert brain.claims == []


async def test_post_is_not_allowed(mkapp, brain) -> None:
    async with client_for(mkapp()) as c:
        r = await c.post("/announcements", headers=auth())
    assert r.status_code == 405
    assert brain.claims == []


async def test_not_a_conversation_turn(mkapp, brain) -> None:
    app = mkapp()
    await poll(app)
    inner = app.app
    memory = await inner.state.conversations.get(NODE_ID)
    assert memory.turns == [] and memory.summary is None
    assert inner.state.background == set()  # no memory-candidate task was scheduled
    assert inner.state.candidates.path is None
    assert brain.asks == []


async def test_announcement_text_never_logged(mkapp, brain, caplog) -> None:
    caplog.set_level(logging.DEBUG)
    await poll(mkapp())
    brain.body = {"announcements": [{"text": "x" * 201}, {"text": TIMER}]}
    # a malformed reply is logged by counts too
    await poll(mkapp())
    text = caplog.text + "".join(str(r.args) for r in caplog.records)
    assert caplog.records
    for secret in (TIMER, REMINDER, "plumber", BRAIN_TOKEN, NODE_TOKEN):
        assert secret not in text
