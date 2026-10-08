"""Task 16 Part 3: voice room assignment ("you're in the office" -> registry room)."""

from __future__ import annotations

import json
import logging

import pytest
from conftest import (
    FAKE_MP3,
    NODE_ID,
    ROOM,
    auth,
    client_for,
    make_note,
    parse_sse,
    turn_headers,
)

from vesper_node.registry import RegistryError
from vesper_node.roomcmd import INVALID_ROOM_REPLY, detect_room_assignment


@pytest.mark.parametrize(
    ("utterance", "room"),
    [
        ("You're in the office.", "office"),
        ("you're in the office", "office"),
        ("You’re in the office", "office"),
        ("You are in the kitchen", "kitchen"),
        ("You're now in the master bedroom.", "master bedroom"),
        ("you are in the living room now", "living room"),
        ("Hey Vesper, you're in the study.", "study"),
        ("Okay, this is the kitchen", "kitchen"),
        ("This is the den!", "den"),
        ("this is now the guest room", "guest room"),
        ("Call this the study.", "study"),
        ("This room is the den.", "den"),
        ("This room is called the lab", "lab"),
        ("this room is called Charity's studio", "charitys studio"),
        ("Call this room the music room", "music room"),
        ("call this room Kevin's lab", "kevins lab"),
        ("Set your room to the garage", "garage"),
        ("Set the room to workshop", "workshop"),
        ("Your room is now the back porch.", "back porch"),
        ("You're in the upstairs bathroom", "upstairs bathroom"),
        ("You're in the home-office", "home office"),
    ],
)
def test_positive_assignments(utterance: str, room: str) -> None:
    got = detect_room_assignment(utterance)
    assert got is not None and got.room == room


@pytest.mark.parametrize(
    "utterance",
    [
        # incidental room mentions
        "Is the office light on?",
        "is the office light on",
        "Turn on the kitchen lights",
        "What's in the study?",
        "what's in the study",
        "Is anyone in the kitchen",
        "Are you in the office?",
        "Turn off the lights in the bedroom",
        "What's the temperature in the living room",
        "I'm in the kitchen",
        "We're in the office",
        "Kevin is in the office",
        "Charity's in the garage",
        "Play music in the den",
        "Is this the kitchen?",
        "Is this the kitchen",
        "You're in the office?",
        "you're in the office, right?",
        "This is the kitchen, isn't it?",
        "You're in the office and turn on the lights",
        "You're in the kitchen so turn on the lights",
        "This is the kitchen light",
        "This is the office printer",
        # ordinary English that shares the shape
        "This is the best day ever",
        "this is the best day",
        "This is the one",
        "This is the end",
        "This is the way",
        "You're in the way",
        "You're in the right",
        "You are in the wrong place",
        "You're in the clear",
        "You're in the doghouse",
        "Call this the final version",
        "call this the end of the day",
        "This is the room",
        "You're in the room",
        "this room is",
        "This room is now",
        "Call this room the",
        "This room is called the",
        "Call this room",
        "Remember that I like my coffee black",
        "Tell me more about that",
        "What room are you in",
        "What room is this",
        "Which room am I in",
        "Set the kitchen lights to fifty percent",
        "Set your timer to ten minutes",
        "",
        "   ",
    ],
)
def test_negative_incidental_mentions(utterance: str) -> None:
    assert detect_room_assignment(utterance) is None


@pytest.mark.parametrize(
    "utterance",
    [
        "This room is called the extraordinarily long and verbose name",  # > 3 words
        "Call this room the " + "x" * 45,  # too long for valid_room
        "This room is called the !!!",
    ],
)
def test_detected_but_invalid_names(utterance: str) -> None:
    got = detect_room_assignment(utterance)
    assert got is None or got.room is None


async def _turn(app):
    async with client_for(app) as c:
        r = await c.post("/turn", content=make_note(), headers=turn_headers())
    return parse_sse(r.text)


async def test_assignment_updates_registry_and_confirms_without_brain(
    build, providers, registry, caplog
) -> None:
    caplog.set_level(logging.DEBUG)
    app = build()
    providers.transcript = "You're in the office."
    events = await _turn(app)
    assert [n for n, _ in events] == [
        "transcript",
        "message_start",
        "text_delta",
        "message_done",
        "timing",
        "done",
    ]
    ev = dict(events)
    assert ev["text_delta"]["text"] == "Got it — this is the office now."
    assert ev["done"] == {"ok": True}
    assert ev["timing"]["ask_ms"] is None
    assert providers.calls["ask"] == []  # no brain round-trip
    (tts_req,) = providers.calls["tts"]  # spoken through the normal TTS path
    assert json.loads(tts_req.content)["text"] == "Got it — this is the office now."
    async with client_for(app) as c:
        audio = await c.get("/" + ev["message_done"]["audio_url"], headers=auth())
    assert audio.content == FAKE_MP3
    assert registry.listing()["nodes"][NODE_ID]["room"] == "office"
    # the next turn carries the new room context
    providers.transcript = "Turn on the light"
    await _turn(app)
    (ask,) = providers.calls["ask"]
    assert json.loads(ask.content)["text"].startswith("[Vesper node in the office] ")
    # room changes are not conversation history
    assert app.app.state.conversations.get_sync(NODE_ID).turns[0].user == "Turn on the light"
    assert len(app.app.state.conversations.get_sync(NODE_ID).turns) == 1
    assert "You're in the office" not in caplog.text


async def test_invalid_room_name_is_refused_by_voice(build, providers, registry) -> None:
    providers.transcript = "Call this room the " + "x" * 45
    ev = dict(await _turn(build()))
    assert ev["text_delta"]["text"] == INVALID_ROOM_REPLY
    assert ev["done"] == {"ok": True}
    assert providers.calls["ask"] == []
    assert registry.listing()["nodes"][NODE_ID]["room"] == ROOM  # unchanged


async def test_incidental_mention_goes_to_brain(build, providers, registry) -> None:
    providers.transcript = "Is the office light on?"
    ev = dict(await _turn(build()))
    assert ev["done"] == {"ok": True}
    assert len(providers.calls["ask"]) == 1
    assert registry.listing()["nodes"][NODE_ID]["room"] == ROOM


async def test_registry_failure_is_an_internal_error(build, providers, registry) -> None:
    app = build()
    providers.transcript = "You're in the office"

    def boom(*_a, **_k):
        raise RegistryError("node registry is malformed")

    registry.set_room = boom  # type: ignore[method-assign]
    ev = dict(await _turn(app))
    assert ev["error"]["code"] == "internal"
    assert ev["done"] == {"ok": False}
    assert providers.calls["ask"] == []
