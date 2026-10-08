"""Voice room assignment (task 16, Part 3): "you're in the office" -> registry room.

Conservative by design. Only a **whole utterance** that is a clear assignment fires; a room
mentioned inside any other sentence never does ("is the office light on", "turn on the kitchen
lights", "what's in the study"). Two pattern families:

* **Explicit room wording** (the sentence itself says *room*): "this room is the den",
  "this room is called the study", "call this room the lab", "set your room to the office",
  "your room is the kitchen". Any name that passes ``valid_room`` is accepted.
* **Short phrasings** that are also ordinary English: "you're in the office", "you are now
  in the kitchen", "this is the kitchen", "call this the study". These fire only when the
  name ends in a room noun from :data:`ROOM_NOUNS`, so "you're in the way" or "this is the
  best day" never match.

A trailing question mark ("you're in the office?") is a question, not an assignment. Names
are at most :data:`MAX_ROOM_WORDS` words, lower-cased, apostrophes dropped ("Kevin's office"
-> "kevins office"), then checked with the registry's ``valid_room``.
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

_FILLER = re.compile(r"^(?:(?:hey|ok|okay|so|um|uh|right|alright|vesper)[,!.]?\s+)+", re.IGNORECASE)
_NAME = r"(?P<name>[a-z0-9' -]+?)"
_NOW = r"(?:\s+now)?"
# Explicit "room" wording: any valid name.
_EXPLICIT = [
    re.compile(rf"^this room is(?: now)?(?: called)?(?: the)? {_NAME}{_NOW}$"),
    re.compile(rf"^call this room(?: the)? {_NAME}{_NOW}$"),
    re.compile(rf"^set (?:your|the|this) room to(?: the)? {_NAME}{_NOW}$"),
    re.compile(rf"^your room is(?: now)?(?: the)? {_NAME}{_NOW}$"),
]
# Ordinary-English phrasings: the name must end in a room noun.
_GATED = [
    re.compile(rf"^(?:you're|youre|you are)(?: now)? in the {_NAME}{_NOW}$"),
    re.compile(rf"^this is(?: now)? the {_NAME}{_NOW}$"),
    re.compile(rf"^call this the {_NAME}{_NOW}$"),
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


def detect_room_assignment(text: str) -> RoomAssignment | None:
    """A :class:`RoomAssignment` if the whole utterance assigns this node's room, else None."""
    s = _clean_utterance(text)
    if not s:
        return None
    for pattern in _EXPLICIT:
        m = pattern.fullmatch(s)
        if m:
            name = m.group("name").strip()
            if not set(name.split()) - _NOT_A_NAME:
                return None
            return RoomAssignment(normalize_room(name))
    for pattern in _GATED:
        m = pattern.fullmatch(s)
        if m:
            name = m.group("name").strip()
            words = name.replace("'", "").replace("-", " ").split()
            if not words or words[-1] not in ROOM_NOUNS or words == ["room"]:
                return None
            if len(words) > MAX_ROOM_WORDS:
                return None
            return RoomAssignment(normalize_room(name))
    return None


def confirmation(room: str) -> str:
    return f"Got it — this is the {room} now."


INVALID_ROOM_REPLY = (
    "Sorry, I can't use that as a room name. Try a short name, like: you're in the office."
)
