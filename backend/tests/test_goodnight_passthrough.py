"""Issue #40 (vesper-voice): Mini inherits the goodnight protocol BY CONSTRUCTION.

Mini has no goodnight code: "goodnight" is an ordinary node turn, forwarded to Little's /ask
(channel "node"), whose brain gates it and runs the shared Studio executor. These tests pin that
nothing here intercepts, retries, or reimplements it. Tests only; no runtime code changed.
"""

from __future__ import annotations

import ast
import json
import re
from pathlib import Path

import httpx
import pytest
from conftest import NODE_ID, ROOM, client_for, make_note, parse_sse, turn_headers

from vesper_node.roomcmd import detect_room_assignment

GOODNIGHT_REPLY = (
    "Goodnight — blinds are closing, all twelve lights are off, and the Frame was already off."
)


async def post_turn(app) -> httpx.Response:
    async with client_for(app) as c:
        return await c.post("/turn", content=make_note(), headers=turn_headers())


def _counting_ask(providers, actions):
    async def ask(request: httpx.Request) -> httpx.Response:
        providers.calls["ask"].append(request)
        return httpx.Response(200, json={"text": providers.reply, "actions": actions})

    return ask


async def test_goodnight_reply_is_spoken_verbatim_and_actions_are_counted(
    build, providers, caplog
) -> None:
    providers.transcript = "goodnight"
    providers.reply = GOODNIGHT_REPLY
    providers.ask = _counting_ask(providers, [{"kind": "blinds"}, {"kind": "lights"}])
    caplog.set_level("INFO")
    events = parse_sse((await post_turn(build())).text)
    ev = dict(events)
    assert ev["text_delta"]["text"] == GOODNIGHT_REPLY  # the brain's sentence, untouched
    assert ev["done"] == {"ok": True}
    (req,) = providers.calls["ask"]  # exactly one forward to the brain
    assert json.loads(req.content) == {
        "text": f"[Vesper node in the {ROOM}] goodnight",
        "device_id": NODE_ID,
        "channel": "node",
    }
    assert re.search(r"actions=2\b", caplog.text)
    # the spoken text is spoken, so it was sent to TTS exactly as the brain wrote it
    (tts,) = providers.calls["tts"]
    assert GOODNIGHT_REPLY in tts.content.decode()


@pytest.mark.parametrize("status", [500, 502, 503, 504])
async def test_brain_failure_is_the_existing_error_path_with_one_post(
    build, providers, status
) -> None:
    providers.transcript = "goodnight protocol"
    providers.ask_status = status
    events = parse_sse((await post_turn(build())).text)
    assert [n for n, _ in events] == ["transcript", "error", "timing", "done"]
    assert dict(events)["error"]["code"] == "ask_failed"
    assert len(providers.calls["ask"]) == 1  # never retried: a retry could re-run the protocol
    assert providers.calls["tts"] == []


@pytest.mark.parametrize(
    "exc", [httpx.ReadTimeout, httpx.ConnectError, httpx.RemoteProtocolError, httpx.ConnectTimeout]
)
async def test_brain_transport_failure_is_one_post_and_never_silence(build, providers, exc) -> None:
    providers.transcript = "goodnight"

    async def broken(request: httpx.Request) -> httpx.Response:
        providers.calls["ask"].append(request)
        raise exc("boom", request=request)

    providers.ask = broken
    events = parse_sse((await post_turn(build())).text)
    assert [n for n, _ in events] == ["transcript", "error", "timing", "done"]
    assert dict(events)["error"]["code"] == "ask_failed"
    assert len(providers.calls["ask"]) == 1  # a reset may follow delivery: never re-ask
    assert providers.calls["tts"] == []


@pytest.mark.parametrize(
    "utterance",
    [
        "goodnight",
        "Good night.",
        "goodnight protocol",
        "good night, you're in the office",
        "goodnight, you're in the kitchen",
        "ok vesper goodnight",
    ],
)
def test_roomcmd_never_claims_goodnight(utterance: str) -> None:
    assert detect_room_assignment(utterance) is None


async def test_goodnight_with_a_room_phrase_still_goes_to_the_brain(build, providers) -> None:
    providers.transcript = "good night, you're in the office"
    events = parse_sse((await post_turn(build())).text)
    assert dict(events)["text_delta"]["text"] == providers.reply
    assert len(providers.calls["ask"]) == 1


def test_import_graph_nothing_in_vesper_node_reaches_the_executor() -> None:
    """Parity by construction: no import of vesper_voice, no executor call, no secrets."""
    src = Path(__file__).resolve().parents[1] / "src" / "vesper_node"
    files = sorted(src.rglob("*.py"))
    assert files
    for path in files:
        tree = ast.parse(path.read_text(), filename=str(path))
        for node in ast.walk(tree):
            mods: list[str] = []
            if isinstance(node, ast.Import):
                mods = [a.name for a in node.names]
            elif isinstance(node, ast.ImportFrom):
                mods = [node.module or ""]
            for mod in mods:
                assert not mod.startswith("vesper_voice"), f"{path.name} imports {mod}"
                assert "goodnight" not in mod.lower(), f"{path.name} imports {mod}"
        text = path.read_text().lower()
        assert "bin/goodnight" not in text, path.name
        assert "goodnight" not in text, path.name
