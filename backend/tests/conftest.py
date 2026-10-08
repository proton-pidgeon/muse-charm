"""Shared fixtures: keyless settings + mocked providers (no network anywhere in verify)."""

from __future__ import annotations

import asyncio
import json
from dataclasses import dataclass, field
from typing import Any

import httpx
import pytest

from vesper_node.app import create_app
from vesper_node.config import Settings
from vesper_node.wav import wav_header

NODE_TOKEN = "node-token-" + "n" * 40
BRAIN_TOKEN = "brain-token-" + "b" * 40
ELEVEN_KEY = "sk_" + "e" * 48
DEEPGRAM_KEY = "d" * 40
VOICE_ID = "VoiceId12345678"
NODE_ID = "homelink-aabbccddeeff"
TRANSCRIPT = "Hello Vesper what is two plus two"
REPLY = "Two plus two is four, my favourite owl fact."
FAKE_MP3 = b"ID3\x04\x00\x00\x00\x00\x00\x00" + b"\xff\xfb\x90\x64" + b"\x00" * 600


def make_note(seconds: float = 1.0, *, streaming: bool = True) -> bytes:
    pcm = b"\x01\x00" * int(16_000 * seconds)
    return wav_header(len(pcm), streaming=streaming) + pcm


def auth(token: str = NODE_TOKEN) -> dict[str, str]:
    return {"Authorization": f"Bearer {token}"}


def turn_headers(**extra: str) -> dict[str, str]:
    return {**auth(), "X-Node-Id": NODE_ID, "Content-Type": "audio/wav", **extra}


def parse_sse(text: str) -> list[tuple[str, dict[str, Any]]]:
    events = []
    for frame in text.split("\n\n"):
        name, data = None, None
        for line in frame.split("\n"):
            if line.startswith(":") or not line:
                continue
            if line.startswith("event: "):
                name = line[len("event: ") :]
            elif line.startswith("data: "):
                data = json.loads(line[len("data: ") :])
        if name is not None:
            events.append((name, data))
    return events


@dataclass
class Providers:
    """Programmable fake ElevenLabs STT/TTS, Deepgram and brain."""

    transcript: str = TRANSCRIPT
    reply: str = REPLY
    eleven_stt_status: int = 200
    deepgram_status: int = 200
    deepgram_transcript: str = TRANSCRIPT
    ask_status: int = 200
    tts_status: int = 200
    tts_delay_s: float = 0.0
    ask_gate: asyncio.Event | None = None
    calls: dict[str, list[httpx.Request]] = field(
        default_factory=lambda: {"eleven_stt": [], "deepgram": [], "ask": [], "tts": []}
    )

    async def stt(self, request: httpx.Request) -> httpx.Response:
        if request.url.host == "api.deepgram.com":
            self.calls["deepgram"].append(request)
            body = {
                "results": {
                    "channels": [{"alternatives": [{"transcript": self.deepgram_transcript}]}]
                }
            }
            return httpx.Response(self.deepgram_status, json=body)
        self.calls["eleven_stt"].append(request)
        return httpx.Response(self.eleven_stt_status, json={"text": self.transcript})

    async def ask(self, request: httpx.Request) -> httpx.Response:
        self.calls["ask"].append(request)
        if self.ask_gate is not None:
            await self.ask_gate.wait()
        return httpx.Response(self.ask_status, json={"text": self.reply, "actions": []})

    async def tts(self, request: httpx.Request) -> httpx.Response:
        self.calls["tts"].append(request)
        if self.tts_delay_s:
            await asyncio.sleep(self.tts_delay_s)
        return httpx.Response(self.tts_status, content=FAKE_MP3)


def make_settings(**overrides: Any) -> Settings:
    base: dict[str, Any] = {
        "node_token": NODE_TOKEN,
        "brain_url": "http://127.0.0.1:8790",
        "brain_token": BRAIN_TOKEN,
        "elevenlabs_api_key": ELEVEN_KEY,
        "deepgram_api_key": None,
        "tts_voice_id": VOICE_ID,
    }
    base.update(overrides)
    return Settings(**base)


@pytest.fixture
def providers() -> Providers:
    return Providers()


@pytest.fixture
def build(providers: Providers):
    def _build(settings: Settings | None = None, **kwargs: Any):
        return create_app(
            settings or make_settings(),
            stt_transport=httpx.MockTransport(providers.stt),
            brain_transport=httpx.MockTransport(providers.ask),
            tts_transport=httpx.MockTransport(providers.tts),
            **kwargs,
        )

    return _build


def client_for(app: Any) -> httpx.AsyncClient:
    return httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://node.test")
