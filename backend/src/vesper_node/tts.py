"""Reply TTS for nodes, following the ``phone_tts.py`` discipline.

Mirrors ``vesper-voice/server/src/vesper_voice/phone_tts.py``. Per Kevin's Q1 decision it uses
the phone brain's ElevenLabs voice (``VESPER_PHONE_TTS_VOICE_ID``):

* ElevenLabs ``eleven_flash_v2_5``, ``output_format=mp3_22050_32``, a 4 s hard timeout, and
  input capped at 500 chars (cut at a word boundary, plus ``...``);
* :class:`AudioCache`: an in-memory LRU keyed by ``sha256(voice|model|text)``, bounded in
  entries and bytes, with in-flight de-duplication of identical texts;
* :class:`AudioStore`: capability ids (``secrets.token_urlsafe(18)``, 144 bits) mapped to MP3
  bytes, with a 10-minute TTL and at most 512 ids. **Memory only, never on disk.** Unlike the
  phone door, the ``GET /audio`` route is *also* bearer-gated, because the firmware can send
  headers;
* ``VESPER_NODE_TTS=off`` is the kill switch. Any failure means ``audio_url`` is null and the
  turn still succeeds.

Logs carry sizes, timings, cache hits, and at most the last 4 chars of an audio id. They never
carry the reply text, a full id, or the API key.
"""

from __future__ import annotations

import asyncio
import hashlib
import logging
import re
import secrets
import time
from collections import OrderedDict
from collections.abc import Callable
from dataclasses import dataclass

import httpx

log = logging.getLogger("vesper_node.tts")

ELEVENLABS_BASE_URL = "https://api.elevenlabs.io"
DEFAULT_MODEL = "eleven_flash_v2_5"
OUTPUT_FORMAT = "mp3_22050_32"
TTS_TIMEOUT_S = 4.0
MAX_TTS_CHARS = 500
ELLIPSIS = "..."
MAX_AUDIO_BYTES = 2 * 1024 * 1024
AUDIO_TTL_S = 10 * 60
MAX_AUDIO_IDS = 512
MAX_CACHE_ENTRIES = 128
MAX_CACHE_BYTES = 16 * 1024 * 1024
AUDIO_ID_BYTES = 18  # -> 24 url-safe chars, 144 bits
AUDIO_ID_RE = re.compile(r"^[A-Za-z0-9_-]{22,64}$")


def truncate_for_tts(text: str, limit: int = MAX_TTS_CHARS) -> str:
    text = " ".join(text.split())
    if len(text) <= limit:
        return text
    room = limit - len(ELLIPSIS)
    cut = text[: room + 1]
    space = cut.rfind(" ")
    head = cut[:space] if space >= room // 2 else text[:room]
    return head.rstrip(" ,;:-.") + ELLIPSIS


def cache_key(voice_id: str, model: str, text: str) -> str:
    return hashlib.sha256(f"{voice_id}\x00{model}\x00{text}".encode()).hexdigest()


def id_tail(audio_id: str) -> str:
    return audio_id[-4:]


class AudioCache:
    def __init__(
        self, max_entries: int = MAX_CACHE_ENTRIES, max_bytes: int = MAX_CACHE_BYTES
    ) -> None:
        self._max_entries = max_entries
        self._max_bytes = max_bytes
        self._items: OrderedDict[str, bytes] = OrderedDict()
        self._bytes = 0

    def __len__(self) -> int:
        return len(self._items)

    def get(self, key: str) -> bytes | None:
        audio = self._items.get(key)
        if audio is not None:
            self._items.move_to_end(key)
        return audio

    def put(self, key: str, audio: bytes) -> None:
        if len(audio) > self._max_bytes:
            return
        old = self._items.pop(key, None)
        if old is not None:
            self._bytes -= len(old)
        self._items[key] = audio
        self._bytes += len(audio)
        while len(self._items) > self._max_entries or self._bytes > self._max_bytes:
            _k, dropped = self._items.popitem(last=False)
            self._bytes -= len(dropped)


@dataclass
class _Clip:
    audio: bytes
    expires: float


class AudioStore:
    def __init__(
        self,
        *,
        ttl_s: float = AUDIO_TTL_S,
        max_ids: int = MAX_AUDIO_IDS,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self._ttl_s = ttl_s
        self._max_ids = max_ids
        self._clock = clock
        self._clips: dict[str, _Clip] = {}

    def __len__(self) -> int:
        return len(self._clips)

    def sweep(self) -> None:
        now = self._clock()
        for aid in [k for k, c in self._clips.items() if c.expires <= now]:
            del self._clips[aid]

    def mint(self, audio: bytes) -> str | None:
        """New id, or ``None`` when full (a returned id must stay live for its TTL)."""
        self.sweep()
        if len(self._clips) >= self._max_ids:
            return None
        audio_id = secrets.token_urlsafe(AUDIO_ID_BYTES)
        self._clips[audio_id] = _Clip(audio, self._clock() + self._ttl_s)
        return audio_id

    def get(self, audio_id: str) -> bytes | None:
        if not AUDIO_ID_RE.fullmatch(audio_id):
            return None
        self.sweep()
        clip = self._clips.get(audio_id)
        return clip.audio if clip is not None else None


class TTSError(RuntimeError):
    """ElevenLabs refused or returned something unusable. The message is a short label."""


async def synthesize_once(
    client: httpx.AsyncClient, *, api_key: str, voice_id: str, model: str, text: str
) -> bytes:
    resp = await client.post(
        f"{ELEVENLABS_BASE_URL}/v1/text-to-speech/{voice_id}",
        params={"output_format": OUTPUT_FORMAT},
        headers={"xi-api-key": api_key, "Accept": "audio/mpeg"},
        json={"text": text, "model_id": model},
    )
    if resp.status_code != 200:
        raise TTSError(f"http_{resp.status_code}")
    audio = resp.content
    if not audio:
        raise TTSError("empty")
    if len(audio) > MAX_AUDIO_BYTES:
        raise TTSError("too_large")
    return audio


class NodeTTS:
    """Synthesis + cache + capability-id hosting for node replies."""

    def __init__(
        self,
        *,
        api_key: str | None,
        voice_id: str | None,
        enabled: bool,
        model: str = DEFAULT_MODEL,
        timeout_s: float = TTS_TIMEOUT_S,
        transport: httpx.AsyncBaseTransport | None = None,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.enabled = bool(enabled and api_key and voice_id)
        self._api_key = api_key or ""
        self._voice_id = voice_id or ""
        self._model = model
        self._timeout_s = timeout_s
        self._transport = transport
        self.cache = AudioCache()
        self.store = AudioStore(clock=clock)
        self._client: httpx.AsyncClient | None = None
        self._inflight: dict[str, asyncio.Future[bytes | None]] = {}
        self.http_calls = 0

    def _http(self) -> httpx.AsyncClient:
        if self._client is None:
            self._client = httpx.AsyncClient(
                timeout=httpx.Timeout(self._timeout_s),
                transport=self._transport,
                follow_redirects=False,
            )
        return self._client

    async def synthesize(self, text: str, *, node: str = "-") -> bytes | None:
        """MP3 bytes for ``text`` (cache first). ``None`` on any failure; never raises."""
        if not self.enabled:
            return None
        spoken = truncate_for_tts(text)
        if not spoken:
            return None
        key = cache_key(self._voice_id, self._model, spoken)
        if (hit := self.cache.get(key)) is not None:
            log.info("tts: node=%s cache=hit bytes=%d", node, len(hit))
            return hit
        pending = self._inflight.get(key)
        if pending is not None:
            return await asyncio.shield(pending)
        fut: asyncio.Future[bytes | None] = asyncio.get_running_loop().create_future()
        self._inflight[key] = fut
        started = time.monotonic()
        audio: bytes | None = None
        try:
            self.http_calls += 1
            audio = await asyncio.wait_for(
                synthesize_once(
                    self._http(),
                    api_key=self._api_key,
                    voice_id=self._voice_id,
                    model=self._model,
                    text=spoken,
                ),
                timeout=self._timeout_s,
            )
        except TimeoutError:
            log.warning("tts failed: node=%s reason=timeout", node)
        except TTSError as e:
            log.warning("tts failed: node=%s reason=%s", node, e)
        except Exception as e:  # noqa: BLE001 - a TTS failure only means audio_url=null
            log.warning("tts failed: node=%s reason=%s", node, type(e).__name__)
        finally:
            self._inflight.pop(key, None)
            if not fut.done():
                fut.set_result(audio)
        if audio is not None:
            self.cache.put(key, audio)
            log.info(
                "tts: node=%s cache=miss chars=%d bytes=%d latency=%.2fs",
                node,
                len(spoken),
                len(audio),
                time.monotonic() - started,
            )
        return audio

    async def mint(self, text: str, *, node: str = "-") -> tuple[str, int] | None:
        """``(audio_id, n_bytes)`` for ``text``, or ``None`` (disabled / failed / store full)."""
        audio = await self.synthesize(text, node=node)
        if audio is None:
            return None
        audio_id = self.store.mint(audio)
        if audio_id is None:
            log.warning("tts: node=%s audio store full", node)
            return None
        return audio_id, len(audio)

    def audio(self, audio_id: str) -> bytes | None:
        return self.store.get(audio_id)

    async def aclose(self) -> None:
        if self._client is not None:
            await self._client.aclose()
            self._client = None
