"""Validate the node's note (WAV + 16 kHz mono PCM16) and re-emit canonical forms.

The firmware's ``muse_hatch_wav_header`` (``muse_chat_text.c:40``) streams the note, so it
writes ``0xFFFFFFFF`` for both the RIFF size and the ``data`` size. :func:`parse_note`
accepts that (and any size that overruns the body): ``data`` runs to the end of the body.
See ``docs/node-wire-protocol.md``.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

SAMPLE_RATE = 16_000
CHANNELS = 1
BITS = 16
BYTES_PER_SECOND = SAMPLE_RATE * CHANNELS * BITS // 8  # 32 000
HEADER_BYTES = 44
MIN_STT_MS = 100  # ElevenLabs' minimum input length
_MAX_CHUNKS = 64


class BadAudio(ValueError):
    """The body is not a 16 kHz mono PCM16 RIFF/WAVE note. Message is a short label."""


@dataclass(frozen=True)
class Note:
    pcm: bytes  # little-endian PCM16 samples, even length

    @property
    def duration_ms(self) -> int:
        return len(self.pcm) * 1000 // BYTES_PER_SECOND

    def canonical_wav(self) -> bytes:
        """A 44-byte header with correct sizes + the PCM (for providers that parse sizes)."""
        return wav_header(len(self.pcm)) + self.pcm


def wav_header(data_len: int, *, streaming: bool = False) -> bytes:
    """Canonical 44-byte PCM header; ``streaming=True`` copies the firmware's 0xFFFFFFFF sizes."""
    riff = 0xFFFFFFFF if streaming else 36 + data_len
    data = 0xFFFFFFFF if streaming else data_len
    return (
        b"RIFF"
        + struct.pack("<I", riff)
        + b"WAVEfmt "
        + struct.pack(
            "<IHHIIHH", 16, 1, CHANNELS, SAMPLE_RATE, BYTES_PER_SECOND, CHANNELS * BITS // 8, BITS
        )
        + b"data"
        + struct.pack("<I", data)
    )


def parse_note(body: bytes) -> Note:
    """Validate ``body`` and return its PCM. Raises :class:`BadAudio`."""
    if len(body) < 12 or body[:4] != b"RIFF" or body[8:12] != b"WAVE":
        raise BadAudio("not_riff_wave")
    pos = 12
    fmt_ok = False
    for _ in range(_MAX_CHUNKS):
        if pos + 8 > len(body):
            break
        cid = body[pos : pos + 4]
        (size,) = struct.unpack_from("<I", body, pos + 4)
        start = pos + 8
        if cid == b"fmt ":
            if size < 16 or start + 16 > len(body):
                raise BadAudio("short_fmt")
            tag, ch, rate, byte_rate, align, bits = struct.unpack_from("<HHIIHH", body, start)
            if (tag, ch, rate, bits, align) != (1, CHANNELS, SAMPLE_RATE, BITS, 2):
                raise BadAudio("unsupported_format")
            if byte_rate != BYTES_PER_SECOND:
                raise BadAudio("unsupported_format")
            fmt_ok = True
        elif cid == b"data":
            if not fmt_ok:
                raise BadAudio("data_before_fmt")
            end = len(body) if size == 0xFFFFFFFF or start + size > len(body) else start + size
            pcm = body[start:end]
            if len(pcm) % 2:
                pcm = pcm[:-1]
            return Note(pcm)
        if size == 0xFFFFFFFF or start + size > len(body):
            break
        pos = start + size + (size & 1)  # chunks are word-aligned
    raise BadAudio("no_data" if fmt_ok else "no_fmt")
