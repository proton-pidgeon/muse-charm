"""POST /turn end to end against mocked STT / brain / TTS, plus GET /audio."""

from __future__ import annotations

import asyncio
import json
import logging
from typing import Any

import httpx
import pytest
from conftest import (
    BRAIN_TOKEN,
    DEEPGRAM_KEY,
    ELEVEN_KEY,
    FAKE_MP3,
    NODE_ID,
    NODE_TOKEN,
    REPLY,
    ROOM,
    TRANSCRIPT,
    auth,
    bearer,
    client_for,
    make_note,
    make_settings,
    parse_sse,
    turn_headers,
)

from vesper_node.wav import wav_header


async def post_turn(app, body: bytes | None = None, **headers: str) -> httpx.Response:
    async with client_for(app) as c:
        return await c.post(
            "/turn", content=make_note() if body is None else body, headers=turn_headers(**headers)
        )


def names(events: list[tuple[str, Any]]) -> list[str]:
    return [n for n, _ in events]


async def test_happy_path_event_sequence_and_audio(build, providers) -> None:
    app = build()
    r = await post_turn(app)
    assert r.status_code == 200
    assert r.headers["content-type"].startswith("text/event-stream")
    assert r.headers["x-vesper-node-protocol"] == "1"
    assert r.text.startswith(":")  # priming preamble comment
    events = parse_sse(r.text)
    assert names(events) == [
        "transcript",
        "message_start",
        "text_delta",
        "message_done",
        "timing",
        "done",
    ]
    ev = dict(events)
    assert ev["transcript"] == {"text": TRANSCRIPT}
    assert ev["text_delta"] == {"id": "m1", "text": REPLY}
    assert ev["done"] == {"ok": True}
    url = ev["message_done"]["audio_url"]
    assert url.startswith("audio/") and url.endswith(".mp3")
    assert ev["message_done"]["audio_bytes"] == len(FAKE_MP3)
    timing = ev["timing"]
    assert timing["stt_provider"] == "elevenlabs"
    assert all(isinstance(timing[k], int) for k in ("stt_ms", "ask_ms", "tts_ms", "total_ms"))
    # relative reference against /turn -> /audio/<id>.mp3, bearer-gated
    async with client_for(app) as c:
        audio = await c.get("/" + url, headers=auth())
        again = await c.get("/" + url, headers=auth())
        no_token = await c.get("/" + url)
    assert audio.status_code == 200 and audio.content == FAKE_MP3
    assert audio.headers["content-type"] == "audio/mpeg"
    assert audio.headers["cache-control"] == "no-store"
    assert again.status_code == 200  # re-fetchable within the TTL
    assert no_token.status_code == 401


async def test_brain_request_contract(build, providers) -> None:
    await post_turn(build())
    (req,) = providers.calls["ask"]
    assert str(req.url) == "http://127.0.0.1:8790/ask"
    assert req.headers["authorization"] == f"Bearer {BRAIN_TOKEN}"
    assert json.loads(req.content) == {
        "text": f"[Vesper node in the {ROOM}] {TRANSCRIPT}",
        "device_id": NODE_ID,
        "channel": "node",
    }
    # the node token never leaves the backend
    for reqs in providers.calls.values():
        for r in reqs:
            assert NODE_TOKEN not in str(r.headers) and NODE_TOKEN.encode() not in r.content


async def test_stt_request_is_scribe_v2_batch_on_pcm(build, providers) -> None:
    note = make_note(0.5)
    await post_turn(build(), note)
    (req,) = providers.calls["eleven_stt"]
    assert str(req.url) == "https://api.elevenlabs.io/v1/speech-to-text"
    assert req.headers["xi-api-key"] == ELEVEN_KEY
    body = req.content
    assert b'name="model_id"\r\n\r\nscribe_v2\r\n' in body
    assert b'name="file_format"\r\n\r\npcm_s16le_16\r\n' in body
    assert b'name="language_code"\r\n\r\nen\r\n' in body
    assert note[44:] in body and b"RIFF" not in body  # raw PCM, header stripped


async def test_tts_request_mirrors_phone_tts(build, providers) -> None:
    await post_turn(build())
    (req,) = providers.calls["tts"]
    assert req.url.path == "/v1/text-to-speech/VoiceId12345678"
    assert req.url.params["output_format"] == "mp3_22050_32"
    assert json.loads(req.content) == {"text": REPLY, "model_id": "eleven_flash_v2_5"}


async def test_tts_cache_reuses_audio(build, providers) -> None:
    app = build()
    await post_turn(app)
    await post_turn(app)
    assert len(providers.calls["tts"]) == 1
    assert len(providers.calls["ask"]) == 2


async def test_empty_transcript_skips_brain(build, providers) -> None:
    providers.transcript = "   "
    events = parse_sse((await post_turn(build())).text)
    assert names(events) == ["error", "timing", "done"]
    assert events[0][1]["code"] == "empty_transcript"
    assert events[-1][1] == {"ok": False}
    assert providers.calls["ask"] == []


async def test_too_short_note_skips_stt(build, providers) -> None:
    events = parse_sse((await post_turn(build(), make_note(0.05))).text)
    assert dict(events)["error"]["code"] == "empty_transcript"
    assert providers.calls["eleven_stt"] == []


async def test_transcript_too_long_never_clipped(build, providers) -> None:
    providers.transcript = "word " * 300
    events = parse_sse((await post_turn(build())).text)
    assert dict(events)["error"]["code"] == "transcript_too_long"
    assert providers.calls["ask"] == []


async def test_stt_failure_without_fallback_key(build, providers) -> None:
    providers.eleven_stt_status = 500
    events = parse_sse((await post_turn(build())).text)
    assert dict(events)["error"]["code"] == "stt_failed"
    assert providers.calls["deepgram"] == [] and providers.calls["ask"] == []


async def test_stt_falls_back_to_deepgram_when_keyed(build, providers) -> None:
    providers.eleven_stt_status = 503
    app = build(make_settings(deepgram_api_key=DEEPGRAM_KEY))
    events = parse_sse((await post_turn(app)).text)
    ev = dict(events)
    assert ev["done"] == {"ok": True}
    assert ev["timing"]["stt_provider"] == "deepgram"
    (req,) = providers.calls["deepgram"]
    assert req.url.params["model"] == "nova-3"
    assert req.headers["authorization"] == f"Token {DEEPGRAM_KEY}"
    assert req.content[:4] == b"RIFF" and req.content[40:44] != b"\xff\xff\xff\xff"


async def test_deepgram_as_primary(build, providers) -> None:
    app = build(make_settings(stt_provider="deepgram", deepgram_api_key=DEEPGRAM_KEY))
    ev = dict(parse_sse((await post_turn(app)).text))
    assert ev["timing"]["stt_provider"] == "deepgram"
    assert providers.calls["eleven_stt"] == []


async def test_ask_failure(build, providers) -> None:
    providers.ask_status = 500
    events = parse_sse((await post_turn(build())).text)
    assert names(events) == ["transcript", "error", "timing", "done"]
    assert dict(events)["error"]["code"] == "ask_failed"
    assert providers.calls["tts"] == []


async def test_tts_failure_keeps_text(build, providers) -> None:
    providers.tts_status = 500
    ev = dict(parse_sse((await post_turn(build())).text))
    assert ev["text_delta"]["text"] == REPLY
    assert ev["message_done"]["audio_url"] is None
    assert ev["done"] == {"ok": True}


async def test_tts_hard_timeout(build, providers) -> None:
    providers.tts_delay_s = 1.0
    ev = dict(parse_sse((await post_turn(build(tts_timeout_s=0.05))).text))
    assert ev["message_done"]["audio_url"] is None
    assert ev["timing"]["tts_ms"] < 900


async def test_tts_kill_switch(build, providers) -> None:
    ev = dict(parse_sse((await post_turn(build(make_settings(tts_off=True)))).text))
    assert ev["message_done"]["audio_url"] is None
    assert ev["timing"]["tts_ms"] is None
    assert providers.calls["tts"] == []


@pytest.mark.parametrize(
    ("headers", "status", "code"),
    [
        ({"X-Node-Id": "bad id with spaces"}, 400, "bad_node_id"),
        ({"X-Node-Id": "x" * 129}, 400, "bad_node_id"),
        ({"Content-Type": "application/json"}, 415, "unsupported_media_type"),
        ({"X-Vesper-Node-Protocol": "2"}, 400, "unsupported_protocol"),
    ],
)
async def test_header_validation(build, providers, headers, status, code) -> None:
    r = await post_turn(build(), **headers)
    assert (r.status_code, r.json()) == (status, {"error": code})
    assert providers.calls["eleven_stt"] == []


async def test_missing_node_id(build) -> None:
    async with client_for(build()) as c:
        r = await c.post(
            "/turn", content=make_note(), headers={**bearer(), "Content-Type": "audio/wav"}
        )
    assert (r.status_code, r.json()) == (400, {"error": "bad_node_id"})


@pytest.mark.parametrize(
    "body",
    [
        b"not a wav at all, definitely not",
        wav_header(32000).replace(b"\x80\x3e\x00\x00", b"\x44\xac\x00\x00", 1) + b"\0" * 100,
        b"RIFF\xff\xff\xff\xffWAVEdata\xff\xff\xff\xff" + b"\0" * 100,
    ],
)
async def test_bad_audio_422(build, providers, body) -> None:
    r = await post_turn(build(), body)
    assert (r.status_code, r.json()) == (422, {"error": "bad_audio"})


async def test_declared_too_large_413(build, providers) -> None:
    r = await post_turn(build(max_body_bytes=1000), make_note(1.0))
    assert (r.status_code, r.json()) == (413, {"error": "too_large"})
    assert providers.calls["eleven_stt"] == []


async def test_streamed_too_large_stops_reading(build, providers) -> None:
    """Chunked upload (no Content-Length): cut off at the first chunk over the cap."""
    pulled = 0
    chunks = [make_note(0.01)] + [b"\0" * 1000] * 50

    async def receive() -> dict[str, Any]:
        nonlocal pulled
        pulled += 1
        return {"type": "http.request", "body": chunks[pulled - 1], "more_body": True}

    sent: list[dict[str, Any]] = []

    async def send(m: dict[str, Any]) -> None:
        sent.append(m)

    scope = {
        "type": "http",
        "asgi": {"version": "3.0"},
        "http_version": "1.1",
        "method": "POST",
        "scheme": "http",
        "path": "/turn",
        "raw_path": b"/turn",
        "query_string": b"",
        "root_path": "",
        "headers": [(k.lower().encode(), v.encode()) for k, v in turn_headers().items()]
        + [(b"transfer-encoding", b"chunked")],
        "client": ("127.0.0.1", 5555),
        "server": ("127.0.0.1", 8796),
    }
    await build(max_body_bytes=5000)(scope, receive, send)
    start = next(m for m in sent if m["type"] == "http.response.start")
    assert start["status"] == 413
    assert pulled <= 6  # ~5 KB cap / 1 KB chunks: stopped early, never drained all 51


async def test_upload_timeout_408(build) -> None:
    async def receive() -> dict[str, Any]:
        await asyncio.sleep(5)
        return {"type": "http.request", "body": b"", "more_body": False}

    sent: list[dict[str, Any]] = []

    async def send(m: dict[str, Any]) -> None:
        sent.append(m)

    scope = {
        "type": "http",
        "asgi": {"version": "3.0"},
        "http_version": "1.1",
        "method": "POST",
        "scheme": "http",
        "path": "/turn",
        "raw_path": b"/turn",
        "query_string": b"",
        "root_path": "",
        "headers": [(k.lower().encode(), v.encode()) for k, v in turn_headers().items()],
        "client": ("127.0.0.1", 5555),
        "server": ("127.0.0.1", 8796),
    }
    await build(upload_timeout_s=0.05)(scope, receive, send)
    assert next(m for m in sent if m["type"] == "http.response.start")["status"] == 408


async def test_busy_503_and_slot_released(build, providers) -> None:
    providers.ask_gate = asyncio.Event()
    app = build(max_concurrent=1)
    async with client_for(app) as c:
        first = asyncio.create_task(c.post("/turn", content=make_note(), headers=turn_headers()))
        for _ in range(100):
            await asyncio.sleep(0.01)
            if providers.calls["ask"]:
                break
        busy = await c.post("/turn", content=make_note(), headers=turn_headers())
        providers.ask_gate.set()
        r1 = await first
        r3 = await c.post("/turn", content=make_note(), headers=turn_headers())
    assert (busy.status_code, busy.json()) == (503, {"error": "busy"})
    assert busy.headers["retry-after"] == "2"
    assert dict(parse_sse(r1.text))["done"] == {"ok": True}
    assert r3.status_code == 200


async def test_audio_ids_expire(build, providers) -> None:
    now = [1000.0]
    app = build(clock=lambda: now[0])
    url = dict(parse_sse((await post_turn(app)).text))["message_done"]["audio_url"]
    async with client_for(app) as c:
        assert (await c.get("/" + url, headers=auth())).status_code == 200
        now[0] += 10 * 60 + 1
        gone = await c.get("/" + url, headers=auth())
    assert (gone.status_code, gone.json()) == (404, {"error": "not_found"})


@pytest.mark.parametrize(
    "path",
    ["/audio/" + "A" * 24 + ".mp3", "/audio/short.mp3", "/audio/../../etc/passwd", "/audio/x"],
)
async def test_audio_misses_are_identical_404(build, path) -> None:
    async with client_for(build()) as c:
        r = await c.get(path, headers=auth())
    assert (r.status_code, r.json()) == (404, {"error": "not_found"})


async def test_firmware_shaped_and_canonical_wav_both_accepted(build, providers) -> None:
    for streaming in (True, False):
        r = await post_turn(build(), make_note(0.3, streaming=streaming))
        assert dict(parse_sse(r.text))["done"] == {"ok": True}


async def test_transcript_and_reply_never_logged(build, providers, caplog) -> None:
    """Q4: transcripts are transient. No transcript/reply text or token in any log line."""
    caplog.set_level(logging.DEBUG)
    app = build(make_settings(deepgram_api_key=DEEPGRAM_KEY))
    await post_turn(app)  # happy path
    providers.ask_status = 502
    await post_turn(app)  # ask failure after a transcript
    providers.ask_status = 200
    providers.tts_status = 500
    await post_turn(app)  # tts failure
    providers.eleven_stt_status = 500
    await post_turn(app)  # fallback STT path
    providers.transcript = providers.deepgram_transcript = "word " * 300
    await post_turn(app)  # too long
    async with client_for(app) as c:
        await c.post("/turn", content=make_note(), headers=turn_headers(Authorization="Bearer x"))
    text = caplog.text + "".join(str(r.args) for r in caplog.records)
    assert caplog.records, "expected some log lines"
    for needle in (TRANSCRIPT, REPLY, "two plus two", "owl", "word word", NODE_TOKEN, BRAIN_TOKEN):
        assert needle not in text, needle
    assert ELEVEN_KEY not in text and DEEPGRAM_KEY not in text
