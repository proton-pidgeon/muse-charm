"""Batch speech-to-text for a finished push-to-talk note.

Provider selection follows ``vesper-voice/server/src/vesper_voice/pipeline.py::build_stt``:
``VESPER_STT_PROVIDER`` (``elevenlabs`` | ``deepgram``, default ``elevenlabs``). Two
differences:

* The node uploads a **whole note**, so this module uses the *batch* REST APIs, not the
  streaming ones: ElevenLabs Scribe v2 (``POST /v1/speech-to-text``, ``model_id=scribe_v2``)
  and Deepgram Nova-3 prerecorded (``POST /v1/listen?model=nova-3``). The ElevenLabs endpoint
  and model id were checked against the ElevenLabs API reference on 2026-10-07.
* If the primary fails, the other provider is tried, but only when its key is configured
  (:meth:`vesper_node.config.Settings.stt_chain`).

Logs carry the provider, status codes, byte/char counts and latency. They never carry the
transcript or the key.
"""

from __future__ import annotations

import logging
import time
from dataclasses import dataclass

import httpx

from .wav import Note

log = logging.getLogger("vesper_node.stt")

ELEVENLABS_STT_URL = "https://api.elevenlabs.io/v1/speech-to-text"
ELEVENLABS_STT_MODEL = "scribe_v2"
DEEPGRAM_STT_URL = "https://api.deepgram.com/v1/listen"
DEEPGRAM_STT_MODEL = "nova-3"
STT_TIMEOUT_S = 10.0
MAX_TRANSCRIPT_CHARS = 4000  # anything longer from STT is treated as a failure


class STTError(RuntimeError):
    """A provider refused or returned something unusable. The message is a short label."""


@dataclass(frozen=True)
class Transcript:
    text: str
    provider: str
    latency_s: float
    fallback_used: bool = False


async def elevenlabs_transcribe(
    client: httpx.AsyncClient, *, api_key: str, note: Note, language: str
) -> str:
    """Scribe v2 batch on raw PCM (``file_format=pcm_s16le_16``: 16 kHz mono s16le)."""
    resp = await client.post(
        ELEVENLABS_STT_URL,
        headers={"xi-api-key": api_key},
        data={
            "model_id": ELEVENLABS_STT_MODEL,
            "file_format": "pcm_s16le_16",
            "language_code": language.split("-", 1)[0].lower(),
            "tag_audio_events": "false",
        },
        files={"file": ("note.pcm", note.pcm, "application/octet-stream")},
    )
    if resp.status_code != 200:
        raise STTError(f"http_{resp.status_code}")
    try:
        text = resp.json().get("text")
    except (ValueError, AttributeError):
        raise STTError("bad_json") from None
    if not isinstance(text, str):
        raise STTError("no_text")
    return text


async def deepgram_transcribe(
    client: httpx.AsyncClient, *, api_key: str, note: Note, language: str
) -> str:
    """Nova-3 prerecorded on a canonical WAV (correct header sizes)."""
    resp = await client.post(
        DEEPGRAM_STT_URL,
        params={
            "model": DEEPGRAM_STT_MODEL,
            "language": language,
            "smart_format": "true",
            "punctuate": "true",
        },
        headers={"Authorization": f"Token {api_key}", "Content-Type": "audio/wav"},
        content=note.canonical_wav(),
    )
    if resp.status_code != 200:
        raise STTError(f"http_{resp.status_code}")
    try:
        alt = resp.json()["results"]["channels"][0]["alternatives"][0]
        text = alt["transcript"]
    except (ValueError, KeyError, IndexError, TypeError):
        raise STTError("bad_json") from None
    if not isinstance(text, str):
        raise STTError("no_text")
    return text


_PROVIDERS = {"elevenlabs": elevenlabs_transcribe, "deepgram": deepgram_transcribe}


class STT:
    """Provider chain with a per-request hard timeout."""

    def __init__(
        self,
        *,
        chain: list[tuple[str, str]],  # [(provider, api_key)], primary first
        language: str,
        timeout_s: float = STT_TIMEOUT_S,
        transport: httpx.AsyncBaseTransport | None = None,
    ) -> None:
        self._chain = chain
        self._language = language
        self._client = httpx.AsyncClient(
            timeout=httpx.Timeout(timeout_s), transport=transport, follow_redirects=False
        )

    @property
    def providers(self) -> list[str]:
        return [p for p, _ in self._chain]

    async def transcribe(self, note: Note, *, node: str = "-") -> Transcript:
        """Transcript text (may be empty). Raises :class:`STTError` if every provider failed."""
        started = time.monotonic()
        for i, (provider, key) in enumerate(self._chain):
            t0 = time.monotonic()
            try:
                text = await _PROVIDERS[provider](
                    self._client, api_key=key, note=note, language=self._language
                )
            except STTError as e:
                log.warning("stt failed: node=%s provider=%s reason=%s", node, provider, e)
                continue
            except httpx.TimeoutException:
                log.warning("stt failed: node=%s provider=%s reason=timeout", node, provider)
                continue
            except Exception as e:  # noqa: BLE001 - try the next provider
                log.warning(
                    "stt failed: node=%s provider=%s reason=%s", node, provider, type(e).__name__
                )
                continue
            text = " ".join(text.split())
            if len(text) > MAX_TRANSCRIPT_CHARS:
                log.warning("stt failed: node=%s provider=%s reason=too_long", node, provider)
                continue
            log.info(
                "stt ok: node=%s provider=%s audio_ms=%d chars=%d latency=%.2fs",
                node,
                provider,
                note.duration_ms,
                len(text),
                time.monotonic() - t0,
            )
            return Transcript(text, provider, time.monotonic() - started, fallback_used=i > 0)
        raise STTError("all_providers_failed")

    async def aclose(self) -> None:
        await self._client.aclose()
