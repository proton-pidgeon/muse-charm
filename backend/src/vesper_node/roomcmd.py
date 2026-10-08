"""Voice room assignment (task 16, Part 3): "you're in the office" -> registry room.

Conservative by design. Only a **whole utterance** that is a clear assignment fires; a room
mentioned inside any other sentence never does ("is the office light on", "turn on the kitchen
lights", "what's in the study"). Two kinds of match:

* **Open vocabulary** (any name that passes ``valid_room``) only behind an unambiguous naming
  marker: "this room is (now) called X", "this room is (now) the X", "call this room the X",
  "call this room Kevin's lab" (a possessive name), "set your/the/this room to the X",
  "your room is (now) the X".
* **Room-noun gated** everything else that shares the shape with ordinary English: "you're in
  the X", "this is the X", "call this the X", and the marker-less *room* phrasings "this room
  is X", "call this room X", "set the room to X", "your room is X". These fire only when the
  name ends in a room noun from :data:`ROOM_NOUNS`, so "this room is cold", "set the room to
  seventy", "call this room service", "you're in the way" never match.

Names that are numbers or measurements ("70 degrees", "72") or end in a state/thermostat word
("cold", "dark", "a mess", "the worst") are never rooms, with or without a marker. A trailing
question mark ("you're in the office?") is a question, not an assignment. Names are at most
:data:`MAX_ROOM_WORDS` words, lower-cased, apostrophes dropped ("Kevin's office" -> "kevins
office"), then checked with the registry's ``valid_room``.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

from .registry import valid_room

MAX_ROOM_WORDS = 3

ROOM_NOUNS = frozenset(
    {
        "attic",
        "basement",
        "bathroom",
        "bedroom",
        "cellar",
        "closet",
        "conservatory",
        "den",
        "entryway",
        "foyer",
        "garage",
        "greenhouse",
        "gym",
        "hall",
        "hallway",
        "kitchen",
        "kitchenette",
        "lanai",
        "laundry",
        "library",
        "loft",
        "lounge",
        "mudroom",
        "nursery",
        "office",
        "pantry",
        "patio",
        "playroom",
        "porch",
        "room",
        "shed",
        "studio",
        "study",
        "sunroom",
        "workshop",
    }
)

# Words that cannot be a whole room name ("this room is now", "call this room the").
_NOT_A_NAME = frozenset({"a", "an", "the", "called", "now", "room", "is", "this", "it"})
# A name ending in one of these is a state, a complaint or a thermostat setting, not a room
# ("this room is the worst", "set the room to the cool setting").
_NOT_A_ROOM_ENDING = frozenset(
    {
        "best",
        "bright",
        "chilly",
        "clean",
        "cold",
        "cool",
        "dark",
        "degrees",
        "dirty",
        "empty",
        "free",
        "freezing",
        "hot",
        "loud",
        "mess",
        "messy",
        "mode",
        "noisy",
        "occupied",
        "percent",
        "quiet",
        "ready",
        "setting",
        "stuffy",
        "temperature",
        "warm",
        "worst",
    }
)
# "70", "70 degrees", "72f", "50%": never a room.
_MEASUREMENT = re.compile(r"^\d+(?:\s*(?:[a-z]+|%|°))?$")

_FILLER = re.compile(r"^(?:(?:hey|ok|okay|so|um|uh|right|alright|vesper)[,!.]?\s+)+", re.IGNORECASE)
_NAME = r"(?P<name>[a-z0-9' -]+?)"
_NOW = r"(?:\s+now)?"
_OPEN, _GATED, _POSSESSIVE = "open", "gated", "possessive"
# (pattern, kind). "open": any valid name. "gated": the name must end in a room noun.
# "possessive": open only for a possessive name ("Kevin's lab"), else gated.
_RULES: list[tuple[re.Pattern[str], str]] = [
    # explicit naming markers: open vocabulary
    (re.compile(rf"^this room is{_NOW} called(?: the)? {_NAME}{_NOW}$"), _OPEN),
    (re.compile(rf"^this room is{_NOW} the {_NAME}{_NOW}$"), _OPEN),
    (re.compile(rf"^call this room the {_NAME}{_NOW}$"), _OPEN),
    (re.compile(rf"^set (?:your|the|this) room to the {_NAME}{_NOW}$"), _OPEN),
    (re.compile(rf"^your room is{_NOW} the {_NAME}{_NOW}$"), _OPEN),
    # "room" wording without a marker: ordinary English too ("this room is cold")
    (re.compile(rf"^this room is{_NOW} {_NAME}{_NOW}$"), _GATED),
    (re.compile(rf"^call this room {_NAME}{_NOW}$"), _POSSESSIVE),
    (re.compile(rf"^set (?:your|the|this) room to {_NAME}{_NOW}$"), _GATED),
    (re.compile(rf"^your room is{_NOW} {_NAME}{_NOW}$"), _GATED),
    # ordinary-English phrasings
    (re.compile(rf"^(?:you're|youre|you are)(?: now)? in the {_NAME}{_NOW}$"), _GATED),
    (re.compile(rf"^this is(?: now)? the {_NAME}{_NOW}$"), _GATED),
    (re.compile(rf"^call this the {_NAME}{_NOW}$"), _GATED),
]


@dataclass(frozen=True)
class RoomAssignment:
    """A detected assignment. ``room`` is None when the spoken name is not a valid room."""

    room: str | None


def _clean_utterance(text: str) -> str | None:
    s = text.strip().replace("’", "'")
    if s.endswith("?"):
        return None
    s = _FILLER.sub("", s)
    s = s.rstrip(" .!").strip().lower()
    return re.sub(r"\s+", " ", s)


def normalize_room(name: str) -> str | None:
    """Spoken name -> registry room (``valid_room``), or None."""
    name = re.sub(r"\s+", " ", name.replace("'", "").replace("-", " ")).strip()
    if not name or len(name.split(" ")) > MAX_ROOM_WORDS:
        return None
    return name if valid_room(name) else None


def _never_a_room(name: str) -> bool:
    """A number, a measurement, a filler-only name or a state word: not a room, go to the brain."""
    words = name.replace("'", "").replace("-", " ").split()
    if not words or not set(words) - _NOT_A_NAME:
        return True
    if _MEASUREMENT.fullmatch(name) or any(w.isdigit() for w in words):
        return True
    return words[-1] in _NOT_A_ROOM_ENDING


def _ends_in_room_noun(name: str) -> bool:
    words = name.replace("'", "").replace("-", " ").split()
    return bool(words) and words[-1] in ROOM_NOUNS and words != ["room"]


def detect_room_assignment(text: str) -> RoomAssignment | None:
    """A :class:`RoomAssignment` if the whole utterance assigns this node's room, else None.

    ``RoomAssignment(None)`` means a clear assignment with an unusable name (too long, bad
    characters): the caller speaks a refusal. None means "not an assignment": the brain gets
    the turn as usual.
    """
    s = _clean_utterance(text)
    if not s:
        return None
    for pattern, kind in _RULES:
        m = pattern.fullmatch(s)
        if not m:
            continue
        name = m.group("name").strip()
        if _never_a_room(name):
            return None
        if kind == _POSSESSIVE and re.search(r"[a-z]'s\b", name):
            kind = _OPEN
        if kind != _OPEN and not _ends_in_room_noun(name):
            return None
        if len(name.replace("-", " ").split()) > MAX_ROOM_WORDS and kind != _OPEN:
            return None
        return RoomAssignment(normalize_room(name))
    return None


def confirmation(room: str) -> str:
    return f"Got it — this is the {room} now."


INVALID_ROOM_REPLY = (
    "Sorry, I can't use that as a room name. Try a short name, like: you're in the office."
)
