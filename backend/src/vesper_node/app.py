"""Vesper node backend (task 07 / B1): push-to-talk note -> STT -> ``/ask`` -> TTS, over SSE.

    vesper-node                  # serve (run by launchd as com.vesper.node)
    vesper-node check-config     # report settings by name (never values); exit 78 if unusable
    vesper-node claim CODE --room ROOM            # approve a node's on-screen claim code
    vesper-node nodes list|add|set-room|allow-shared-token|revoke|remove ...   (see cli.py)
    vesper-node firmware publish BIN|status|withdraw  # node firmware updates (task 13)

The wire protocol is ``docs/node-wire-protocol.md`` (v1). Backend routes (Peggy strips the
``/vesper-node`` prefix before proxying):

* ``POST /turn``: one push-to-talk turn. The body is a raw WAV note and the reply is an SSE
  stream.
* ``GET /audio/{id}.mp3``: the reply MP3 (capability id, 10-minute TTL, bearer-gated too).
* ``GET /healthz``: unauthenticated ``{"ok": true}``.
* ``POST /claim/start``, ``POST /claim/poll``: the node side of the claim flow (task 08).
* ``POST /admin/claim``: approve a claim code (only if ``VESPER_NODE_ADMIN_TOKEN`` is set).
* ``GET /firmware/manifest``, ``GET /firmware/<sha256>.bin``: the published node firmware
  (task 13, :mod:`vesper_node.firmware`), same node auth as ``/turn``.

Auth is checked before the body is read, as in ``vesper-voice/.../brain_service.py``.
:class:`BearerAuth` is a pure-ASGI middleware that checks ``Authorization: Bearer
<VESPER_NODE_TOKEN>`` with ``hmac.compare_digest`` on every route except ``GET /healthz``,
and answers 401 **without ever calling** ``receive``. The body is never consumed on a bad
token. The same middleware then does the per-route second factor, still before the body
(task 08, conflict C2): ``/turn``, ``/audio`` and ``/firmware`` (task 13) need a registered
``X-Node-Id`` plus its ``X-Node-Credential`` (403 ``node_unauthorized`` otherwise);
``/admin/*`` needs
``X-Vesper-Node-Admin: <VESPER_NODE_ADMIN_TOKEN>``.

Transcripts are never logged (Kevin's Q4 decision). Transcript and reply text go to the node
over the SSE stream, and (task 16, Kevin's request 2026-10-08) into the node's conversation
memory on the Studio (:mod:`vesper_node.conversation`): the last 15 exchanges plus a session
summary, prepended to the next ``/ask`` so follow-ups like "tell me more about that" work.
Logs carry the node id, counts, char lengths, latencies, providers and status codes.

Turn pipeline: STT -> voice room assignment (:mod:`vesper_node.roomcmd`: "you're in the
office" updates the registry and is confirmed locally, no brain call) -> session check
(idle > 15 min: compress the old transcript into an extractive summary, no brain call) ->
``/ask`` with room + history
-> record the exchange -> TTS.

Long-term memory (task 17, :mod:`vesper_node.candidates`): after a turn has finished (its
``done`` event is queued and its slot released), a background task judges the exchange with
deterministic patterns in a worker thread and, if it is significant ("remember that …"),
appends one candidate line to ``~/memory/node-candidates.jsonl`` for Vesper to review. That
path adds no latency to the turn, cannot fail it, never calls the brain and never reads
curated memory. Room-assignment turns and failed turns are never considered.
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
from .brain import MAX_TEXT_CHARS, AskError, BrainClient, with_room_context
from .candidates import CandidateQueue
from .config import ConfigError, Settings, load_settings
from .conversation import (
    HISTORY_BUDGET,
    ConversationStore,
    NodeMemory,
    build_ask_text,
    extractive_summary,
)
from .firmware import IMAGE_NAME_RE, VERSION_RE, FirmwareError, FirmwareStore
from .registry import NODE_ID_RE, POLL_INTERVAL_S, ClaimError, NodeAuth, Registry, RegistryError
from .roomcmd import INVALID_ROOM_REPLY, confirmation, detect_room_assignment
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
AUDIO_NAME_RE = re.compile(r"^([A-Za-z0-9_-]{22,64})\.mp3$")
WAV_TYPES = frozenset({"audio/wav", "audio/x-wav", "audio/wave", "audio/vnd.wave"})
OPEN_ROUTES = frozenset({("GET", "/healthz"), ("HEAD", "/healthz")})
CLAIM_ROUTES = frozenset({("POST", "/claim/start"), ("POST", "/claim/poll")})
ADMIN_PREFIX = "/admin/"
ADMIN_HEADER = b"x-vesper-node-admin"
MAX_ADMIN_BODY_BYTES = 1024
NODE_AUTH_SCOPE_KEY = "vesper_node.auth"
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


def _single_header(scope: Scope, name: bytes) -> tuple[str | None, bool]:
    """``(value, ok)``: ok is False when the header is repeated (ambiguous -> refuse)."""
    values = [v for k, v in scope.get("headers", []) if k.lower() == name]
    if len(values) > 1:
        return None, False
    return (values[0].decode("latin-1") if values else None), True


def _is_node_route(method: str, path: str) -> bool:
    return (method == "POST" and path == "/turn") or (
        method in {"GET", "HEAD"} and path.startswith(("/audio/", "/firmware/"))
    )


class BearerAuth:
    """Pure-ASGI auth gate. A rejected request's ``receive`` is never called (body unread).

    Layer 1 (every route but ``/healthz``): the shared ``VESPER_NODE_TOKEN`` bearer -> 401.
    Layer 2 (task 08): ``/turn`` + ``/audio``: registered node + credential -> 403;
    ``/claim/*``: well-formed ``X-Node-Id``; ``/admin/*``: admin token -> 403 (404 if the
    admin route is disabled). It also stamps ``X-Vesper-Node-Protocol: 1`` on every response.
    """

    def __init__(
        self, app: ASGIApp, token: str, registry: Registry, admin_token: str | None = None
    ) -> None:
        self.app = app
        self._token = token
        self.registry = registry
        self._admin_token = admin_token

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

        async def reject(status: int, code: str, *extra: tuple[bytes, bytes]) -> None:
            body = json.dumps({"error": code}).encode()
            await stamped_send(
                {
                    "type": "http.response.start",
                    "status": status,
                    "headers": [
                        (b"content-type", b"application/json"),
                        (b"content-length", str(len(body)).encode()),
                        *extra,
                        (b"connection", b"close"),  # the unread body must not poison keep-alive
                    ],
                }
            )
            await stamped_send({"type": "http.response.body", "body": body})

        method, path = scope.get("method", ""), scope.get("path", "")
        if (method, path) in OPEN_ROUTES:
            await self.app(scope, receive, stamped_send)
            return
        header, _ = _single_header(scope, b"authorization")
        if not authorized(header, self._token):
            log.warning("rejected: route=%s missing or invalid bearer token", _route_label(scope))
            await reject(401, "unauthorized", (b"www-authenticate", b"Bearer"))
            return

        if path.startswith(ADMIN_PREFIX):
            if not self._admin_token:
                await reject(404, "not_found")
                return
            presented, ok = _single_header(scope, ADMIN_HEADER)
            if (
                not ok
                or not presented
                or not hmac.compare_digest(presented.strip().encode(), self._admin_token.encode())
            ):
                log.warning("rejected: route=admin missing or invalid admin token")
                await reject(403, "forbidden")
                return
        elif _is_node_route(method, path) or (method, path) in CLAIM_ROUTES:
            node_id, ok = _single_header(scope, b"x-node-id")
            if not ok or node_id is None or not NODE_ID_RE.fullmatch(node_id):
                await reject(400, "bad_node_id")
                return
            if _is_node_route(method, path):
                credential, ok = _single_header(scope, b"x-node-credential")
                result = (
                    self.registry.authenticate(node_id, (credential or "").strip() or None)
                    if ok
                    else NodeAuth(False, node_id, reason="bad_credential")
                )
                if not result.ok:
                    log.warning(
                        "rejected: route=%s node=%s reason=%s",
                        _route_label(scope),
                        node_id,
                        result.reason,
                    )
                    await reject(403, "node_unauthorized")
                    return
                scope[NODE_AUTH_SCOPE_KEY] = result
        await self.app(scope, receive, stamped_send)


def _route_label(scope: Scope) -> str:
    """A log-safe route label: known routes only, never the raw path (it may carry an id)."""
    path = scope.get("path", "")
    if path == "/turn":
        return "turn"
    if path.startswith("/audio/"):
        return "audio"
    if path.startswith("/claim/"):
        return "claim"
    if path.startswith("/firmware/"):
        return "firmware"
    if path.startswith(ADMIN_PREFIX):
        return "admin"
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
    registry: Registry | None = None,
    firmware: FirmwareStore | None = None,
    conversations: ConversationStore | None = None,
    candidates: CandidateQueue | None = None,
) -> BearerAuth:
    """Build the ASGI app (FastAPI wrapped in :class:`BearerAuth`). Transports are for tests."""
    if registry is None:
        if not settings.registry_file:
            raise ValueError("create_app needs a registry (settings.registry_file or registry=)")
        registry = Registry(settings.registry_file)
    if firmware is None and settings.firmware_dir:
        firmware = FirmwareStore(settings.firmware_dir)
    if conversations is None:
        conversations = ConversationStore(
            settings.memory_file, idle_s=settings.session_idle_minutes * 60
        )
    memory = conversations
    if candidates is None:
        candidates = CandidateQueue(settings.candidates_file)
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
            "vesper-node up: stt=%s tts=%s max_concurrent=%d memory=%s idle_s=%d candidates=%s",
            ",".join(stt.providers),
            "on" if tts.enabled else "off",
            max_concurrent,
            "file" if memory.path else "ram",
            memory.idle_s,
            "file" if candidates.path else "ram",
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
    app.state.conversations = memory
    app.state.candidates = candidates
    app.state.background = tasks  # turn tasks + memory-candidate tasks (tests)

    @app.exception_handler(StarletteHTTPException)
    async def http_error(_request: Request, exc: StarletteHTTPException) -> JSONResponse:
        code = {404: "not_found", 405: "method_not_allowed"}.get(exc.status_code, "error")
        return _err(exc.status_code, code)

    @app.get("/healthz")
    async def healthz() -> dict[str, bool]:
        return {"ok": True}

    async def save_memory(node_id: str, state: NodeMemory) -> None:
        try:
            await memory.put(node_id, state)
        except OSError as e:  # keep the turn: memory stays in RAM until the next write
            log.error("node memory write failed: node=%s reason=%s", node_id, type(e).__name__)

    def summarize(node_id: str, old: NodeMemory) -> NodeMemory:
        """Compress an idle session with the extractive summary (no brain call: ``/ask``
        always runs the home-control tool loop, so old utterances must never be replayed to
        it). Lock held."""
        summary = extractive_summary(old)
        log.info(
            "session compressed: node=%s turns=%d summary=extractive chars=%d",
            node_id,
            len(old.turns),
            len(summary or ""),
        )
        return memory.compressed(old, summary)

    async def session_memory(node_id: str) -> NodeMemory:
        """This node's memory for the next ask, compressing an idle session first."""
        current = await memory.get(node_id)
        if memory.is_idle(current, memory.clock()):
            current = summarize(node_id, current)
            await save_memory(node_id, current)
        return current

    async def remember(node_id: str, user: str, reply: str) -> None:
        """Record a completed exchange (re-read under the lock: turns may overlap)."""
        async with memory.lock(node_id):
            current = await memory.get(node_id)
            await save_memory(node_id, memory.append(current, user, reply, memory.clock()))

    async def stage_candidate(
        node_id: str, room: str | None, user: str, reply: str, previous: tuple[str, str] | None
    ) -> None:
        """Task 17: judge + stage off the turn path, in a worker thread. Never raises."""
        try:
            await asyncio.to_thread(
                candidates.consider,
                node_id=node_id,
                room=room,
                user_text=user,
                vesper_reply=reply,
                previous=previous,
            )
        except Exception as e:  # noqa: BLE001 - a memory candidate must never matter to a turn
            log.error("memory candidate failed: node=%s reason=%s", node_id, type(e).__name__)

    def schedule_candidate(*args: Any) -> None:
        task = asyncio.create_task(stage_candidate(*args), name="memory-candidate")
        tasks.add(task)
        task.add_done_callback(tasks.discard)

    async def assign_room(node: NodeAuth, room: str | None) -> bool:
        """Update the registry room. False only if the registry write failed."""
        if room is None:
            log.info("room assignment: node=%s result=invalid_name", node.node_id)
            return True
        try:
            await asyncio.to_thread(registry.set_room, node.node_id, room)
        except ClaimError as e:
            log.warning("room assignment failed: node=%s reason=%s", node.node_id, e.code)
            return False
        except RegistryError as e:
            log.error("room assignment failed: node=%s reason=%s", node.node_id, e)
            return False
        log.info("room assigned: node=%s from=%s to=%s", node.node_id, node.room, room)
        return True

    async def run_turn(note: Any, node: NodeAuth, upload_s: float, q: asyncio.Queue) -> None:
        node_id = node.node_id
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
        mem_info = "-"  # log-only: history turns sent / "room" / "idle"
        exchange: tuple[str, str, tuple[str, str] | None] | None = None  # task 17

        def emit(event: str, data: dict[str, Any]) -> None:
            q.put_nowait(sse(event, data))

        def fail(c: str) -> None:
            nonlocal code
            code = c
            emit("error", {"code": c, "message": ERROR_MESSAGES[c]})

        async def speak(text: str) -> None:
            """The reply events: text, then (if TTS is on) the MP3 URL."""
            emit("message_start", {"id": "m1"})
            emit("text_delta", {"id": "m1", "text": text})
            t = clock()
            minted = await tts.mint(text, node=node_id) if tts.enabled else None
            timing["tts_ms"] = _ms(clock() - t) if tts.enabled else None
            emit(
                "message_done",
                {
                    "id": "m1",
                    "audio_url": f"audio/{minted[0]}.mp3" if minted else None,
                    "audio_bytes": minted[1] if minted else None,
                },
            )

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
            # task 16 Part 3: "you're in the office" is handled here, without the brain
            assignment = detect_room_assignment(heard.text)
            if assignment is not None:
                mem_info = "room"
                if not await assign_room(node, assignment.room):
                    fail("internal")
                    return
                await speak(
                    confirmation(assignment.room) if assignment.room else INVALID_ROOM_REPLY
                )
                ok = True
                return
            # task 08: registry room. Kevin's own words are never clipped: refuse instead.
            if len(with_room_context(heard.text, node.room)) > MAX_TEXT_CHARS:
                fail("transcript_too_long")
                return
            # task 16 Parts 1+2: session check + history (lock released during the ask)
            async with memory.lock(node_id):
                history = await session_memory(node_id)
            ask_text = build_ask_text(
                heard.text, node.room, history, limit=MAX_TEXT_CHARS, budget=HISTORY_BUDGET
            )
            mem_info = str(len(history.turns)) + ("+summary" if history.summary else "")
            t = clock()
            try:
                reply = await brain.ask(ask_text, node_id=node_id)
            except AskError:
                timing["ask_ms"] = _ms(clock() - t)
                fail("ask_failed")  # nothing recorded: there was no reply
                return
            timing["ask_ms"] = _ms(clock() - t)
            await remember(node_id, heard.text, reply.text)
            await speak(reply.text)
            ok = True
            previous = (history.turns[-1].user, history.turns[-1].reply) if history.turns else None
            exchange = (heard.text, reply.text, previous)
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
            if ok and exchange is not None:  # after the turn: zero added latency
                schedule_candidate(node_id, node.room, *exchange)
            log.info(
                "turn done: node=%s room=%s auth=%s ok=%s code=%s memory=%s audio_ms=%d "
                "upload_ms=%s stt_ms=%s ask_ms=%s tts_ms=%s total_ms=%s provider=%s",
                node_id,
                node.room,
                node.via,
                ok,
                code or "-",
                mem_info,
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
        # (bearer + node credential passed in BearerAuth, before any body byte was read)
        node: NodeAuth = request.scope[NODE_AUTH_SCOPE_KEY]
        proto = request.headers.get("x-vesper-node-protocol")
        if proto is not None and proto.strip() != PROTOCOL_VERSION:
            return _err(400, "unsupported_protocol")
        node_id = node.node_id
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
        task = asyncio.create_task(run_turn(note, node, upload_s, q), name="node-turn")
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

    def claim_error(e: ClaimError) -> JSONResponse:
        status = {
            "rate_limited": 429,
            "too_many_pending": 429,
            "claim_locked": 429,
            "claim_not_found": 404,
            "bad_room": 400,
        }.get(e.code, 400)
        headers = {"Cache-Control": "no-store"}
        if e.retry_after:
            headers["Retry-After"] = str(e.retry_after)
        return _err(status, e.code, **headers)

    def registry_down(e: RegistryError) -> JSONResponse:
        log.error("node registry unusable: %s", e)
        return _err(503, "registry_unavailable")

    @app.post("/claim/start")
    async def claim_start(request: Request) -> Response:
        # Shared bearer + well-formed X-Node-Id were checked in BearerAuth. No body is read.
        node_id = request.headers["x-node-id"]
        try:
            started = await asyncio.to_thread(registry.start_claim, node_id)
        except ClaimError as e:
            log.warning("claim start refused: node=%s reason=%s", node_id, e.code)
            return claim_error(e)
        except RegistryError as e:
            return registry_down(e)
        log.info("claim started: node=%s", node_id)
        return JSONResponse(
            {
                "status": "pending",
                "claim_code": started.code,
                "claim_secret": started.secret,
                "expires_in": started.expires_in,
                "poll_interval": POLL_INTERVAL_S,
            },
            headers={"Cache-Control": "no-store"},
        )

    @app.post("/claim/poll")
    async def claim_poll(request: Request) -> Response:
        node_id = request.headers["x-node-id"]
        presented = request.headers.getlist("x-claim-secret")
        secret = presented[0].strip() if len(presented) == 1 else ""
        try:
            got = await asyncio.to_thread(registry.poll, node_id, secret)
        except ClaimError as e:
            log.info("claim poll: node=%s result=%s", node_id, e.code)
            return claim_error(e)
        except RegistryError as e:
            return registry_down(e)
        if got.status == "pending":
            return JSONResponse(
                {
                    "status": "pending",
                    "expires_in": got.expires_in,
                    "poll_interval": POLL_INTERVAL_S,
                },
                status_code=202,
                headers={"Cache-Control": "no-store"},
            )
        return JSONResponse(
            {
                "status": "claimed",
                "node_id": node_id,
                "room": got.room,
                "credential": got.credential,
            },
            headers={"Cache-Control": "no-store"},
        )

    @app.post("/admin/claim")
    async def admin_claim(request: Request) -> Response:
        # Shared bearer AND admin token were checked in BearerAuth, before this body read.
        declared = request.headers.get("content-length")
        if declared is not None and (
            not declared.isdigit() or len(declared) > 6 or int(declared) > MAX_ADMIN_BODY_BYTES
        ):
            return _err(413, "too_large")
        try:
            raw = await asyncio.wait_for(_read_bounded(request, MAX_ADMIN_BODY_BYTES), timeout=10.0)
            body = json.loads(raw)
        except _TooLarge:
            return _err(413, "too_large")
        except Exception:  # noqa: BLE001 - timeout, disconnect or bad JSON
            return _err(400, "bad_request")
        code = body.get("code") if isinstance(body, dict) else None
        room = body.get("room") if isinstance(body, dict) else None
        if not isinstance(code, str) or not isinstance(room, str) or len(code) > 32:
            return _err(400, "bad_request")
        try:
            node_id, replaces = await asyncio.to_thread(registry.approve, code, room.strip())
        except ClaimError as e:
            log.warning("admin claim refused: reason=%s", e.code)
            return claim_error(e)
        except RegistryError as e:
            return registry_down(e)
        return JSONResponse(
            {"node_id": node_id, "room": room.strip(), "replaces_access": replaces},
            headers={"Cache-Control": "no-store"},
        )

    # ---- Firmware updates (task 13) ----

    def firmware_down(e: FirmwareError) -> JSONResponse:
        log.error("firmware store unusable: %s", e.code)
        return _err(503, "firmware_unavailable", **{"Cache-Control": "no-store"})

    @app.get("/firmware/manifest")
    async def firmware_manifest(request: Request) -> Response:
        node: NodeAuth = request.scope[NODE_AUTH_SCOPE_KEY]
        running = request.headers.get("x-node-firmware", "")
        running = running if VERSION_RE.fullmatch(running) else "?"
        try:
            manifest = await asyncio.to_thread(firmware.current) if firmware else None
        except FirmwareError as e:
            return firmware_down(e)
        if manifest is None:
            log.info("firmware check: node=%s running=%s published=none", node.node_id, running)
            return Response(status_code=204, headers={"Cache-Control": "no-store"})
        log.info(
            "firmware check: node=%s running=%s published=%s",
            node.node_id,
            running,
            manifest.version,
        )
        return JSONResponse(manifest.wire(), headers={"Cache-Control": "no-store"})

    @app.get("/firmware/{name:path}")
    async def firmware_image(request: Request, name: str) -> Response:
        node: NodeAuth = request.scope[NODE_AUTH_SCOPE_KEY]
        m = IMAGE_NAME_RE.fullmatch(name)
        try:
            found = await asyncio.to_thread(firmware.image, m.group(1)) if m and firmware else None
        except FirmwareError as e:
            return firmware_down(e)
        if found is None:
            log.info("firmware image: not found node=%s", node.node_id)
            return _err(404, "not_found", **{"Cache-Control": "no-store"})
        manifest, data = found
        log.info(
            "firmware image: served node=%s version=%s bytes=%d",
            node.node_id,
            manifest.version,
            len(data),
        )
        return Response(
            content=data,
            media_type="application/octet-stream",
            headers={"Cache-Control": "no-store", "X-Content-Type-Options": "nosniff"},
        )

    return BearerAuth(app, settings.node_token, registry, settings.admin_token)


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
    print(f"node registry: {s.registry_file} (VESPER_NODE_REGISTRY_FILE)")
    try:
        count = Registry(s.registry_file or "").check()
    except RegistryError as e:
        print(f"config error: {e}", file=sys.stderr)
        return EX_CONFIG
    print(f"  registered nodes: {count}")
    print(f"node firmware: {s.firmware_dir} (VESPER_NODE_FIRMWARE_DIR)")
    try:
        published = FirmwareStore(s.firmware_dir).current() if s.firmware_dir else None
    except FirmwareError as e:
        print(f"config error: firmware store: {e}", file=sys.stderr)
        return EX_CONFIG
    print(f"  published: {published.version if published else 'none'}")
    print(f"node memory: {s.memory_file} (VESPER_NODE_MEMORY_FILE)")
    print(f"  session idle: {s.session_idle_minutes} min (VESPER_NODE_SESSION_IDLE_MINUTES)")
    print(f"memory candidates: {s.candidates_file} (VESPER_NODE_CANDIDATES_FILE)")
    print(
        "admin claim route: "
        + ("on (VESPER_NODE_ADMIN_TOKEN)" if s.admin_token else "off (CLI approval only)")
    )
    print("ok: VESPER_NODE_TOKEN and VESPER_BRAIN_TOKEN present")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if args[:1] == ["check-config"]:
        return check_config()
    if args[:1] in (["claim"], ["nodes"]):
        from .cli import registry_cli

        return registry_cli(args)
    if args[:1] == ["firmware"]:
        from .cli import firmware_cli

        return firmware_cli(args)
    if args and args[:1] != ["serve"]:
        print("usage: vesper-node [serve|check-config|claim|nodes|firmware]", file=sys.stderr)
        return EX_USAGE
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s %(message)s")
    logsafe.install_log_redaction()
    try:
        settings = load_settings()
    except ConfigError as e:
        print(f"config error: {e}", file=sys.stderr)
        return EX_CONFIG
    try:
        nodes = Registry(settings.registry_file or "").check()
    except RegistryError as e:
        print(f"config error: {e}", file=sys.stderr)
        return EX_CONFIG
    log.info("node registry: %d node(s)", nodes)
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
