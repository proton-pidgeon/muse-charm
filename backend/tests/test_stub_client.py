"""The stub client's offline helpers (the live run is `make verify-live`)."""

from __future__ import annotations

import importlib.util
import wave
from pathlib import Path

from vesper_node.wav import parse_note

_SPEC = importlib.util.spec_from_file_location(
    "stub_client", Path(__file__).resolve().parents[1] / "scripts" / "stub_client.py"
)
assert _SPEC and _SPEC.loader
stub = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(stub)


def test_firmware_note_shape(tmp_path: Path) -> None:
    path = tmp_path / "n.wav"
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(b"\x02\x00" * 1600)
    note = stub.firmware_note(path)
    assert note[4:8] == b"\xff\xff\xff\xff" and note[40:44] == b"\xff\xff\xff\xff"
    assert parse_note(note).pcm == b"\x02\x00" * 1600


def test_is_mp3() -> None:
    assert stub.is_mp3(b"ID3\x04rest")
    assert stub.is_mp3(b"\xff\xfb\x90\x64")
    assert not stub.is_mp3(b'{"error": "x"}')
    assert not stub.is_mp3(b"")


def test_summarize() -> None:
    turns = [
        {
            "ok": True,
            "first_text_s": 1.0 + i,
            "client_total_s": 2.0 + i,
            "audio_fetch_s": 0.1,
            "timing": {
                "upload_ms": 5,
                "stt_ms": 300 + i,
                "ask_ms": 1000,
                "tts_ms": 400,
                "total_ms": 1700,
                "stt_provider": "elevenlabs",
            },
        }
        for i in range(5)
    ]
    s = stub.summarize([*turns, {"ok": False}])
    assert s["ok_turns"] == 5 and s["turns"] == 6
    assert s["stt_ms"]["median"] == 0.302 and s["stt_provider"] == "elevenlabs"
