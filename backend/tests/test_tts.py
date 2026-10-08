"""Direct tests for the tts.py primitives (store/cache bounds, truncation, in-flight dedup)."""

from __future__ import annotations

import asyncio

import httpx

from vesper_node.tts import (
    ELLIPSIS,
    MAX_TTS_CHARS,
    AudioCache,
    AudioStore,
    NodeTTS,
    truncate_for_tts,
)


def test_store_full_by_count_returns_none() -> None:
    store = AudioStore(max_ids=2)
    assert store.mint(b"a") and store.mint(b"b")
    assert store.mint(b"c") is None


def test_store_byte_budget_refuses_and_frees_on_expiry() -> None:
    now = [0.0]
    store = AudioStore(ttl_s=10, max_bytes=1000, clock=lambda: now[0])
    assert store.mint(b"x" * 600)
    assert store.mint(b"y" * 500) is None  # would exceed the byte budget
    assert store.mint(b"z" * 400)
    assert store.total_bytes == 1000
    now[0] = 11.0
    assert store.mint(b"w" * 900)
    assert store.total_bytes == 900


def test_cache_evicts_oldest_by_entries_and_bytes() -> None:
    cache = AudioCache(max_entries=2, max_bytes=100)
    cache.put("a", b"1" * 10)
    cache.put("b", b"2" * 10)
    cache.put("c", b"3" * 10)
    assert cache.get("a") is None and len(cache) == 2
    cache.put("d", b"4" * 95)
    assert cache.get("b") is None and cache.get("c") is None and cache.get("d") is not None
    cache.put("big", b"5" * 101)  # larger than the whole budget: not cached
    assert cache.get("big") is None


def test_truncate_for_tts_word_boundary() -> None:
    text = " ".join(["word"] * 150)  # 749 chars
    out = truncate_for_tts(text)
    assert len(out) <= MAX_TTS_CHARS and out.endswith(ELLIPSIS)
    assert out[: -len(ELLIPSIS)].split(" ")[-1] == "word"
    assert truncate_for_tts("  short   reply ") == "short reply"


async def test_concurrent_synthesize_dedups_to_one_http_call() -> None:
    calls = 0

    async def handler(request: httpx.Request) -> httpx.Response:
        nonlocal calls
        calls += 1
        await asyncio.sleep(0.05)
        return httpx.Response(200, content=b"ID3" + b"\x00" * 100)

    tts = NodeTTS(api_key="k", voice_id="v", enabled=True, transport=httpx.MockTransport(handler))
    try:
        a, b = await asyncio.gather(tts.synthesize("same text"), tts.synthesize("same text"))
    finally:
        await tts.aclose()
    assert a == b and a is not None
    assert calls == 1
