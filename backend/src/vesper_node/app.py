"""Vesper node backend (task 07 / B1): push-to-talk note -> STT -> ``/ask`` -> TTS, over SSE.

    vesper-node                  # serve (run by launchd as com.vesper.node)
    vesper-node check-config     # report settings by name (never values); exit 78 if unusable

The wire protocol is ``docs/node-wire-protocol.md`` (v1). Backend routes (Peggy strips the
``/vesper-node`` prefix before proxying):

* ``POST /turn``: one push-to-talk turn. The body is a raw WAV note and the reply is an SSE
  stream.
* ``GET /audio/{id}.mp3``: the reply MP3 (capability id, 10-minute TTL, bearer-gated too).
* ``GET /healthz``: unauthenticated ``{"ok": true}``.

Auth is checked before the body is read, as in ``vesper-voice/.../brain_service.py``.
:class:`BearerAuth` is a pure-ASGI middleware that checks ``Authorization: Bearer
<VESPER_NODE_TOKEN>`` with ``hmac.compare_digest`` on every route except ``GET /healthz``,
and answers 401 **without ever calling** ``receive``. The body is never consumed on a bad
token.

Transcripts are transient (Kevin's Q4 decision). Transcript and reply text go to the node
over the SSE stream only. They are never logged or persisted. Logs carry the node id,
counts, latencies, providers and status codes.
"""

from __future__ import annotations

import asyncio
import hmac
import json
import logging
import re
import sys
import time
from collections.abc import Awaitable, Callable, MutableMapping, Sequence
from contextlib import asynccontextmanager, suppress
from typing import Any

import httpx
from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse, Response, StreamingResponse
from starlette.exceptions import HTTPException as StarletteHTTPException

from . import logsafe
from .brain import MAX_TEXT_CHARS, AskError, BrainClient
from .config import ConfigError, Settings, load_settings
from .stt import STT, STTError
from .tts import NodeTTS
from .wav import MIN_STT_MS, BadAudio, parse_note

log = logging.getLogger("vesper_node.app")

EX_USAGE = 64
EX_CONFIG = 78
PROTOCOL_VERSION = "1"
PROTOCOL_HEADER = "X-Vesper-Node-Protocol"
MAX_BODY_BYTES = 512 * 1024  # ~16.4 s of 16 kHz PCM16: the firmware's 15 s max + 250 ms tail
UPLOAD_TIMEOUT_S = 30.0
MAX_CONCURRENT_TURNS = 2
BUSY_RETRY_AFTER_S = 2
SSE_PREAMBLE_BYTES = 2048  # Peggy HTTP/2 edge buffers tiny streams (memory: SSE priming)
SSE_PING_S = 10.0
NODE_ID_RE = re.compile(r"^[A-Za-z0-9._:-]{1,128}$")
AUDIO_NAME_RE = re.compile(r"^([A-Za-z0-9_-]{22,64})\.mp3$")
WAV_TYPES = frozenset({"audio/wav", "audio/x-wav", "audio/wave", "audio/vnd.wave"})
OPEN_ROUTES = frozenset({("GET", "/healthz"), ("HEAD", "/healthz")})

# Fixed, safe captions for SSE `error` events (never derived from user input).
ERROR_MESSAGES = {
    "empty_transcript": "Sorry, I didn't catch that.",
    "transcript_too_long": "Sorry, that was too long for me. Could you say it more briefly?",
    "stt_failed": "Sorry, I couldn't hear that properly. Please try again.",
    # no "try again": the brain may still have run the turn (a retry could toggle twice)
    "ask_failed": "Sorry, I couldn't get an answer. Please check before asking again.",
    "internal": "Sorry, something went wrong on my end.",
}

Scope = MutableMapping[str, Any]
Message = MutableMapping[str, Any]
Receive = Callable[[], Awaitable[Message]]
Send = Callable[[Message], Awaitable[None]]
ASGIApp = Callable[[Scope, Receive, Send], Awaitable[None]]


def authorized(header: str | None, expected: str) -> bool:
    """Constant-time bearer check (``brain_service._authorized``)."""
    if not header or not expected:
        return False
    scheme, _, presented = header.partition(" ")
    if scheme.lower() != "bearer":
        return False
    return hmac.compare_digest(presented.strip().encode(), expected.encode())


class BearerAuth:
    """Pure-ASGI auth gate. A rejected request's ``receive`` is never called (body unread).

    It also stamps ``X-Vesper-Node-Protocol: 1`` on every HTTP response.
    """

    def __init__(self, app: ASGIApp, token: str) -> None:
        self.app = app
        self._token = token

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return

        async def stamped_send(message: Message) -> None:
            if message["type"] == "http.response.start":
                headers = list(message.get("headers", []))
                headers.append((PROTOCOL_HEADER.lower().encode(), PROTOCOL_VERSION.encode()))
                message["headers"] = headers
            await send(message)

        if (scope.get("method", ""), scope.get("path", "")) in OPEN_ROUTES:
            await self.app(scope, receive, stamped_send)
            return
        values = [v for k, v in scope.get("headers", []) if k.lower() == b"authorization"]
        header = values[0].decode("latin-1") if len(values) == 1 else None
        if not authorized(header, self._token):
            log.warning("rejected: route=%s missing or invalid bearer token", _route_label(scope))
            body = json.dumps({"error": "unauthorized"}).encode()
            await stamped_send(
                {
                    "type": "http.response.start",
                    "status": 401,
                    "headers": [
                        (b"content-type", b"application/json"),
                        (b"content-length", str(len(body)).encode()),
                        (b"www-authenticate", b"Bearer"),
                        (b"connection", b"close"),  # the unread body must not poison keep-alive
                    ],
                }
            )
            await stamped_send({"type": "http.response.body", "body": body})
            return
        await self.app(scope, receive, stamped_send)


def _route_label(scope: Scope) -> str:
    """A log-safe route label: known routes only, never the raw path (it may carry an id)."""
    path = scope.get("path", "")
    if path == "/turn":
        return "turn"
    if path.startswith("/audio/"):
        return "audio"
    return "other"


def _err(status: int, code: str, **headers: str) -> JSONResponse:
    return JSONResponse({"error": code}, status_code=status, headers=headers or None)


def sse(event: str, data: dict[str, Any]) -> bytes:
    payload = json.dumps(data, separators=(",", ":"), ensure_ascii=False)
    return f"event: {event}\ndata: {payload}\n\n".encode()


class _TooLarge(Exception):
    pass


async def _read_bounded(request: Request, limit: int) -> bytes:
    """Read the body, aborting at the first chunk that crosses ``limit``."""
    buf = bytearray()
    async for chunk in request.stream():
        buf += chunk
        if len(buf) > limit:
            raise _TooLarge
    return bytes(buf)


def _ms(seconds: float | None) -> int | None:
    return None if seconds is None else round(seconds * 1000)


def create_app(
    settings: Settings,
    *,
    stt_transport: httpx.AsyncBaseTransport | None = None,
    brain_transport: httpx.AsyncBaseTransport | None = None,
    tts_transport: httpx.AsyncBaseTransport | None = None,
    tts_timeout_s: float | None = None,
    clock: Callable[[], float] = time.monotonic,
    max_concurrent: int = MAX_CONCURRENT_TURNS,
    upload_timeout_s: float = UPLOAD_TIMEOUT_S,
    max_body_bytes: int = MAX_BODY_BYTES,
    sse_preamble_bytes: int = SSE_PREAMBLE_BYTES,
    sse_ping_s: float = SSE_PING_S,
) -> ASGIApp:
    """Build the ASGI app (FastAPI wrapped in :class:`BearerAuth`). Transports are for tests."""
    stt = STT(
        chain=[(p, settings.stt_key(p) or "") for p in settings.stt_chain()],
        language=settings.stt_language,
        transport=stt_transport,
    )
    brain = BrainClient(
        ask_url=settings.ask_url, token=settings.brain_token, transport=brain_transport
    )
    tts_kwargs: dict[str, Any] = {} if tts_timeout_s is None else {"timeout_s": tts_timeout_s}
    tts = NodeTTS(
        api_key=settings.elevenlabs_api_key,
        voice_id=settings.tts_voice_id,
        enabled=settings.tts_enabled,
        transport=tts_transport,
        clock=clock,
        **tts_kwargs,
    )
    state = {"inflight": 0}
    tasks: set[asyncio.Task[None]] = set()

    def release() -> None:
        state["inflight"] -= 1

    @asynccontextmanager
    async def lifespan(_app: FastAPI):
        log.info(
            "vesper-node up: stt=%s tts=%s max_concurrent=%d",
            ",".join(stt.providers),
            "on" if tts.enabled else "off",
            max_concurrent,
        )
        yield
        for task in list(tasks):
            task.cancel()
        for task in list(tasks):
            with suppress(asyncio.CancelledError, Exception):
                await task
        await stt.aclose()
        await brain.aclose()
        await tts.aclose()

    app = FastAPI(
        title="vesper-node", docs_url=None, redoc_url=None, openapi_url=None, lifespan=lifespan
    )
    app.state.tts = tts  # tests + ops
    app.state.turn_state = state

    @app.exception_handler(StarletteHTTPException)
    async def http_error(_request: Request, exc: StarletteHTTPException) -> JSONResponse:
        code = {404: "not_found", 405: "method_not_allowed"}.get(exc.status_code, "error")
        return _err(exc.status_code, code)

    @app.get("/healthz")
    async def healthz() -> dict[str, bool]:
        return {"ok": True}

    async def run_turn(note: Any, node_id: str, upload_s: float, q: asyncio.Queue) -> None:
        started = clock()
        timing: dict[str, Any] = {
            "upload_ms": _ms(upload_s),
            "stt_ms": None,
            "ask_ms": None,
            "tts_ms": None,
            "total_ms": None,
            "stt_provider": None,
        }
        ok, code = False, None

        def emit(event: str, data: dict[str, Any]) -> None:
            q.put_nowait(sse(event, data))

        def fail(c: str) -> None:
            nonlocal code
            code = c
            emit("error", {"code": c, "message": ERROR_MESSAGES[c]})

        try:
            if note.duration_ms < MIN_STT_MS:
                fail("empty_transcript")
                return
            t = clock()
            try:
                heard = await stt.transcribe(note, node=node_id)
            except STTError:
                timing["stt_ms"] = _ms(clock() - t)
                fail("stt_failed")
                return
            timing["stt_ms"] = _ms(clock() - t)
            timing["stt_provider"] = heard.provider
            if not heard.text:
                fail("empty_transcript")
                return
            emit("transcript", {"text": heard.text})  # to the node only; never logged
            if len(heard.text) > MAX_TEXT_CHARS:
                fail("transcript_too_long")
                return
            t = clock()
            try:
                reply = await brain.ask(heard.text, node_id=node_id)
            except AskError:
                timing["ask_ms"] = _ms(clock() - t)
                fail("ask_failed")
                return
            timing["ask_ms"] = _ms(clock() - t)
            emit("message_start", {"id": "m1"})
            emit("text_delta", {"id": "m1", "text": reply.text})
            t = clock()
            minted = await tts.mint(reply.text, node=node_id) if tts.enabled else None
            timing["tts_ms"] = _ms(clock() - t) if tts.enabled else None
            emit(
                "message_done",
                {
                    "id": "m1",
                    "audio_url": f"audio/{minted[0]}.mp3" if minted else None,
                    "audio_bytes": minted[1] if minted else None,
                },
            )
            ok = True
        except asyncio.CancelledError:
            code = "cancelled"
            raise
        except Exception as e:  # noqa: BLE001 - never surface internals to the node
            log.error("turn crashed: node=%s reason=%s", node_id, type(e).__name__)
            fail("internal")
        finally:
            timing["total_ms"] = _ms(clock() - started)
            emit("timing", timing)
            emit("done", {"ok": ok})
            q.put_nowait(None)
            release()
            log.info(
                "turn done: node=%s ok=%s code=%s audio_ms=%d upload_ms=%s stt_ms=%s "
                "ask_ms=%s tts_ms=%s total_ms=%s provider=%s",
                node_id,
                ok,
                code or "-",
                note.duration_ms,
                timing["upload_ms"],
                timing["stt_ms"],
                timing["ask_ms"],
                timing["tts_ms"],
                timing["total_ms"],
                timing["stt_provider"] or "-",
            )

    async def stream(q: asyncio.Queue):
        if sse_preamble_bytes > 0:
            yield b":" + b" " * max(0, sse_preamble_bytes - 3) + b"\n\n"
        while True:
            try:
                item = await asyncio.wait_for(q.get(), timeout=sse_ping_s)
            except TimeoutError:
                yield b": ping\n\n"
                continue
            if item is None:
                return
            yield item

    @app.post("/turn")
    async def turn(request: Request) -> Response:
        # (auth already passed in BearerAuth, before any body byte was read)
        proto = request.headers.get("x-vesper-node-protocol")
        if proto is not None and proto.strip() != PROTOCOL_VERSION:
            return _err(400, "unsupported_protocol")
        node_id = request.headers.get("x-node-id", "")
        if not NODE_ID_RE.fullmatch(node_id):
            return _err(400, "bad_node_id")
        ctype = request.headers.get("content-type", "").split(";", 1)[0].strip().lower()
        if ctype not in WAV_TYPES:
            return _err(415, "unsupported_media_type")
        declared = request.headers.get("content-length")
        if declared is not None and (not declared.isdigit() or len(declared) > 12):
            return _err(400, "bad_request")
        if declared is not None and int(declared) > max_body_bytes:
            log.warning("turn rejected: node=%s too_large declared=%s", node_id, declared)
            return _err(413, "too_large")
        if state["inflight"] >= max_concurrent:
            log.warning("turn rejected: node=%s busy", node_id)
            return _err(503, "busy", **{"Retry-After": str(BUSY_RETRY_AFTER_S)})
        state["inflight"] += 1  # reserved; released by run_turn's finally or below
        t_upload = clock()
        try:
            body = await asyncio.wait_for(
                _read_bounded(request, max_body_bytes), timeout=upload_timeout_s
            )
            note = parse_note(body)
        except _TooLarge:
            release()
            log.warning("turn rejected: node=%s too_large (streamed)", node_id)
            return _err(413, "too_large")
        except TimeoutError:
            release()
            log.warning("turn rejected: node=%s upload_timeout", node_id)
            return _err(408, "upload_timeout")
        except BadAudio as e:
            release()
            log.warning("turn rejected: node=%s bad_audio=%s", node_id, e)
            return _err(422, "bad_audio")
        except Exception as e:  # noqa: BLE001 - client disconnect mid-upload etc.
            release()
            log.warning("turn rejected: node=%s upload_failed=%s", node_id, type(e).__name__)
            return _err(400, "bad_request")
        upload_s = clock() - t_upload
        log.info(
            "turn accepted: node=%s bytes=%d audio_ms=%d", node_id, len(body), note.duration_ms
        )
        q: asyncio.Queue = asyncio.Queue()
        # The pipeline runs as its own task, so it finishes (and releases its slot) even if
        # the node disconnects mid-stream. The brain does not cancel on disconnect either.
        task = asyncio.create_task(run_turn(note, node_id, upload_s, q), name="node-turn")
        tasks.add(task)
        task.add_done_callback(tasks.discard)
        return StreamingResponse(
            stream(q),
            media_type="text/event-stream; charset=utf-8",
            headers={"Cache-Control": "no-store", "X-Accel-Buffering": "no"},
        )

    @app.get("/audio/{name:path}")
    async def audio(name: str) -> Response:
        m = AUDIO_NAME_RE.fullmatch(name)
        clip = tts.audio(m.group(1)) if m else None
        if clip is None or m is None:
            log.info("audio: not found")
            return _err(404, "not_found", **{"Cache-Control": "no-store"})
        log.info("audio: served id=...%s bytes=%d", m.group(1)[-4:], len(clip))
        return Response(
            content=clip,
            media_type="audio/mpeg",
            headers={"Cache-Control": "no-store", "X-Content-Type-Options": "nosniff"},
        )

    return BearerAuth(app, settings.node_token)


def check_config() -> int:
    try:
        s = load_settings()
    except ConfigError as e:
        print(f"config error: {e}", file=sys.stderr)
        return EX_CONFIG
    print(f"env files: {', '.join(s.env_files)}")
    print(f"listen: {s.host} port {s.port}")
    print(f"brain: {s.ask_url}")
    print(f"stt chain: {', '.join(s.stt_chain())} (primary {s.stt_provider})")
    print("tts: " + ("on" if s.tts_enabled else ("off (VESPER_NODE_TTS)" if s.tts_off else "off")))
    print("ok: VESPER_NODE_TOKEN and VESPER_BRAIN_TOKEN present")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if args[:1] == ["check-config"]:
        return check_config()
    if args and args[:1] != ["serve"]:
        print("usage: vesper-node [serve|check-config]", file=sys.stderr)
        return EX_USAGE
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s %(message)s")
    logsafe.install_log_redaction()
    try:
        settings = load_settings()
    except ConfigError as e:
        print(f"config error: {e}", file=sys.stderr)
        return EX_CONFIG
    app = create_app(settings)

    import uvicorn

    uvicorn.run(
        app,
        host=settings.host,
        port=settings.port,
        log_config=None,
        access_log=False,  # request lines add nothing; the app logs what matters
        server_header=False,
        proxy_headers=False,
        timeout_keep_alive=5,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
