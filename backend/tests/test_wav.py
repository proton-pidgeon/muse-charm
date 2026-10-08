from __future__ import annotations

import io
import struct
import wave

import pytest

from vesper_node.wav import BadAudio, parse_note, wav_header


def test_firmware_header_matches_muse_hatch_wav_header() -> None:
    """Byte-for-byte what muse_chat_text.c:40 writes for a 16 kHz note."""
    h = wav_header(0, streaming=True)
    assert len(h) == 44
    assert h[:4] == b"RIFF" and h[4:8] == b"\xff\xff\xff\xff" and h[8:16] == b"WAVEfmt "
    assert struct.unpack("<IHHIIHH", h[16:36]) == (16, 1, 1, 16000, 32000, 2, 16)
    assert h[36:40] == b"data" and h[40:44] == b"\xff\xff\xff\xff"


def test_streaming_sizes_take_rest_of_body() -> None:
    pcm = b"\x01\x02" * 1600
    note = parse_note(wav_header(0, streaming=True) + pcm)
    assert note.pcm == pcm
    assert note.duration_ms == 100


def test_canonical_and_odd_trailing_byte() -> None:
    pcm = b"\x01\x02" * 100
    assert parse_note(wav_header(len(pcm)) + pcm + b"\x09").pcm == pcm  # trailing junk ignored
    assert parse_note(wav_header(0, streaming=True) + pcm + b"\x09").pcm == pcm  # odd byte dropped


def test_overrunning_data_size_is_clamped() -> None:
    pcm = b"\x00\x00" * 10
    assert parse_note(wav_header(10_000) + pcm).pcm == pcm


def test_stdlib_wave_output_with_extra_chunk() -> None:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(b"\x05\x00" * 800)
    raw = buf.getvalue()
    # insert a LIST chunk (odd length -> padded) between fmt and data
    with_list = raw[:36] + b"LIST" + struct.pack("<I", 3) + b"abc\x00" + raw[36:]
    assert parse_note(with_list).pcm == b"\x05\x00" * 800


def test_canonical_wav_roundtrip() -> None:
    pcm = b"\x01\x00" * 160
    out = parse_note(wav_header(0, streaming=True) + pcm).canonical_wav()
    with wave.open(io.BytesIO(out)) as w:
        assert (w.getnchannels(), w.getframerate(), w.getsampwidth()) == (1, 16000, 2)
        assert w.readframes(1000) == pcm


@pytest.mark.parametrize(
    ("fields", "label"),
    [
        ((16, 3, 1, 16000, 64000, 4, 32), "unsupported_format"),  # float
        ((16, 1, 2, 16000, 64000, 4, 16), "unsupported_format"),  # stereo
        ((16, 1, 1, 44100, 88200, 2, 16), "unsupported_format"),  # 44.1 kHz
        ((16, 1, 1, 16000, 16000, 1, 8), "unsupported_format"),  # 8-bit
        ((16, 1, 1, 16000, 12345, 2, 16), "unsupported_format"),  # inconsistent byte rate
    ],
)
def test_rejects_wrong_format(fields, label) -> None:
    body = b"RIFF\xff\xff\xff\xffWAVEfmt " + struct.pack("<IHHIIHH", *fields) + b"data" + b"\0" * 8
    with pytest.raises(BadAudio, match=label):
        parse_note(body)


@pytest.mark.parametrize(
    "body",
    [b"", b"RIFF", b"RIFX\0\0\0\0WAVE", b"RIFF\0\0\0\0AVI ", b"RIFF\0\0\0\0WAVE"],
)
def test_rejects_non_wave(body) -> None:
    with pytest.raises(BadAudio):
        parse_note(body)


def test_rejects_missing_data_chunk() -> None:
    with pytest.raises(BadAudio, match="no_data"):
        parse_note(wav_header(0)[:36])
