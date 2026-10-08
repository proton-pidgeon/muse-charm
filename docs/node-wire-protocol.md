# Vesper node ↔ backend wire protocol (v1)

**Status:** v1, written by task 07 (B1) before the backend was built, as conflict C1 in
`specs/00-conflicts.md` requires. Task 09 (F1, firmware backend) consumes this doc and may
amend it. Any amendment that changes the wire shape bumps the protocol version (see
*Versioning*).
**Default shape:** `docs/vesper-node-architecture.md` §2. The node does an HTTPS note upload
(WAV header + 16 kHz PCM16, raw POST). The reply is an SSE stream carrying the text plus a
per-message TTS MP3 URL.
**Interaction model (v1):** push-to-talk. The node records the whole note while the talk
button is held, then uploads it in one request after release. There is no streaming upload
and no wake word.

## Decisions this doc records (Kevin, 2026-10-07)

| Open question | Decision | Effect on the protocol / backend |
|---|---|---|
| Q1 TTS voice | Reuse the phone brain's ElevenLabs voice: `VESPER_PHONE_TTS_VOICE_ID`, model `eleven_flash_v2_5`, `phone_tts.py` discipline. No new voice. | `message_done.audio_url` is an MP3 in that voice. |
| Q4 transcripts | **Transient only.** The backend never logs or persists transcripts or reply text. | Logs carry lengths, latencies, provider, status codes only. Tests enforce this. MP3s live in memory only, with a TTL. |
| Q6 LiveKit spike | No spike. Build the HTTP shim. | This doc. |
| PTT vs wake word | PTT for v1. | Whole-note upload, below. |

## Endpoints and path mapping

The node talks to Peggy. Peggy's `/vesper-node/*` handle (task 12, human-gated) checks the
bearer at the edge, runs `uri strip_prefix /vesper-node`, and reverse-proxies to the backend
over 6PN. **The backend's own routes have no `/vesper-node` prefix.**

| Node-facing URL (via Peggy) | Backend route (after `strip_prefix`) | Auth |
|---|---|---|
| `POST https://peggy.fly.dev/vesper-node/turn` | `POST /turn` | node bearer |
| `GET https://peggy.fly.dev/vesper-node/audio/{id}.mp3` | `GET /audio/{id}.mp3` | node bearer |
| *(not routed by Peggy)* | `GET /healthz` | none. Returns `{"ok": true}` only, no detail. |

The task text names the route `POST /vesper-node/turn`. That is the node-facing path. The
backend serves it as `/turn` because Peggy strips the prefix. Peggy must not route
`/vesper-node/healthz` to the backend, though it would be harmless if it did.

Backend default bind: `VESPER_NODE_HOST=::`, `VESPER_NODE_PORT=8796`. `::` listens on every
interface: the 6PN address `fdaa:3e:60bd:a7b:9016:9c37:ac65:d902` (for the Peggy handle) and
loopback (for the stub client and health checks). Set `VESPER_NODE_HOST` to the 6PN address
to restrict it to 6PN only. Loopback health checks then stop working.

## Authentication

- Every route except `GET /healthz` requires `Authorization: Bearer <VESPER_NODE_TOKEN>`.
- The backend checks the token **before it reads a single byte of the body**, in a pure-ASGI
  middleware, with a constant-time compare (`hmac.compare_digest`). This is the same
  discipline as `vesper-voice/server/src/vesper_voice/brain_service.py`, and the check runs
  again on every route. Peggy also checks the same token at the edge, so a bad token is
  normally refused there and never proxied.
- A missing or wrong token gets `401` with `WWW-Authenticate: Bearer` and body
  `{"error": "unauthorized"}`. The request body is never consumed.
- `VESPER_NODE_TOKEN` must be at least 32 characters. It lives in
  `~/.config/vesper-voice/node.env` (mode 600). The backend refuses to start if the token is
  missing or short, or if the file is group/other-readable.
- The node token is **never forwarded**. The backend calls the brain with its own
  `VESPER_BRAIN_TOKEN`. One node token can reach `/vesper-node/*` only (Caddy matcher scope).
- Per-node credentials and the claim flow belong to task 08 (conflict C2). v1 uses one shared
  node token plus the `X-Node-Id` header. Task 08 may add a per-node credential next to it.

## `POST /turn`: one push-to-talk turn

### Request

```
POST /vesper-node/turn HTTP/1.1
Host: peggy.fly.dev
Authorization: Bearer <VESPER_NODE_TOKEN>
X-Node-Id: homelink-<mac>
X-Vesper-Node-Protocol: 1            (optional; if present it must be "1")
Content-Type: audio/wav              (audio/x-wav and audio/wave are accepted too)
Content-Length: <n>                  (or Transfer-Encoding: chunked)
Accept: text/event-stream

<44-byte WAV header><16 kHz mono PCM16 little-endian samples>
```

- **`X-Node-Id`** is required and must match `^[A-Za-z0-9._:-]{1,128}$`, the same rule `/ask`
  applies to `device_id`. The firmware sends `identity_node_id()`, i.e. `homelink-<mac>`.
  The backend passes it to `/ask` as `device_id`.
- **Body** is the raw note, exactly what the firmware already builds in `open_note`
  (`muse_hatch_wav_header`, `muse_chat_text.c:40`). There is **no base64 and no JSON
  envelope**. The backend checks:
  - `RIFF` … `WAVE`, then a `fmt ` chunk with format tag 1 (PCM), 1 channel, 16000 Hz, 16
    bits per sample, block align 2. Anything else gets `422 bad_audio`.
  - The firmware writes `0xFFFFFFFF` for both the RIFF size and the `data` size because it
    streams the note before it knows the length. The backend **accepts that**, and accepts
    any size that overruns the body. The `data` chunk then runs to the end of the body. An
    odd trailing byte is dropped. Unknown chunks before `data` (`LIST`, `FLLR`, …) are
    skipped.
  - **Size cap 524 288 bytes** (512 KiB ≈ 16.4 s of audio, which covers the firmware's 15 s
    max plus its 250 ms release tail). A larger `Content-Length` gets `413 too_large` before
    any body read. A chunked body is cut off **while it streams**, at the first chunk that
    crosses the cap.
  - The whole body must arrive within 30 s, or the request gets `408 upload_timeout`.
  - A note under 100 ms (ElevenLabs' minimum) is not sent to STT. It ends as
    `error: empty_transcript`.

### Responses that come before the stream (plain HTTP, JSON body `{"error": "<code>"}`)

Every response carries `X-Vesper-Node-Protocol: 1`.

| Status | `error` | When |
|---|---|---|
| 401 | `unauthorized` | missing or wrong bearer (body never read) |
| 400 | `bad_node_id` | `X-Node-Id` missing or malformed |
| 400 | `unsupported_protocol` | `X-Vesper-Node-Protocol` present and not `1` |
| 415 | `unsupported_media_type` | `Content-Type` is not a WAV type |
| 413 | `too_large` | declared or streamed body over the cap |
| 408 | `upload_timeout` | body did not finish within 30 s |
| 422 | `bad_audio` | not RIFF/WAVE, wrong format, or no `data` chunk |
| 503 | `busy` | concurrency limit reached (2 turns in flight). Has `Retry-After: 2`. Sent before the body is read. |
| 404 / 405 | `not_found` / `method_not_allowed` | wrong path or method (auth still runs first) |

The node should show a short error caption for these and must not retry automatically. The
only exception is `503 busy`, which it may retry once after `Retry-After`.

### Reply: `200 text/event-stream`

Once the note is accepted, the backend answers `200` with
`Content-Type: text/event-stream; charset=utf-8`, `Cache-Control: no-store`,
`X-Accel-Buffering: no`, and streams standard SSE frames:

```
event: <name>
data: <one line of compact UTF-8 JSON>
<blank line>
```

**Priming preamble.** The stream opens with an SSE **comment** of about 2 KB (`: ` + padding).
Peggy's HTTP/2 edge buffers small responses, so without a preamble short replies may never
reach the client (memory note "SSE priming behind Peggy"). Per the SSE spec, the node must
**ignore every line that starts with `:`**. The backend may also send `: ping` comments.

Events, in order:

| Event | Data | Firmware use (`muse_hatch_*` seam, task 09/10) |
|---|---|---|
| `transcript` | `{"text": str}` | Optional. What STT heard, to show as the user caption. For display only. The backend never logs it. A node may ignore it. |
| `message_start` | `{"id": "m1"}` | Same role as stock `delta.message_start`. Open a reply message, set the avatar to THINKING→SPEAKING, clear the caption. |
| `text_delta` | `{"id": "m1", "text": str}` | Same role as stock `text_append`. Append to the caption (`muse_hatch_turn_caption`). v1 sends **one** delta with the whole reply, because `/ask` does not stream. The node must still accept any number of deltas and concatenate them. |
| `message_done` | `{"id": "m1", "audio_url": str \| null, "audio_bytes": int \| null}` | Same role as stock `message_done`, plus the empty TTS slot (`muse_chat_session.cpp:1503-1517`). If `audio_url` is non-null, `GET` it with the same bearer and feed the MP3 into the existing `tts_data`/`decode`/`muse_hatch_turn_read` path (task 10). If it is null, keep the stock behaviour: caption paced over silence. |
| `timing` | `{"upload_ms", "stt_ms", "ask_ms", "tts_ms", "total_ms": int \| null, "stt_provider": str \| null}` | Diagnostics only. Durations, never text. The node may ignore it. |
| `error` | `{"code": str, "message": str}` | End of turn with no reply. Show `message` (a short, fixed, safe caption) and set the avatar to ERROR. Codes are listed below. |
| `done` | `{"ok": bool}` | Always the **last** event. Close the stream and return to IDLE. |

Normal turn: `transcript` → `message_start` → `text_delta` → `message_done` → `timing` →
`done {"ok": true}`.
Failed turn: (`transcript`)? → `error` → `timing` → `done {"ok": false}`.

The text appears before the audio. `text_delta` is sent as soon as `/ask` returns, so the
caption shows while the TTS request is still running. `message_done` follows when the MP3 is
ready, or when TTS fails, which takes at most 4 s.

**`audio_url` is a relative reference**, `audio/<id>.mp3`, resolved against the turn URL
(RFC 3986). Behind Peggy, `…/vesper-node/turn` resolves to `…/vesper-node/audio/<id>.mp3`.
Direct to the backend, `/turn` resolves to `/audio/<id>.mp3`. The firmware can replace the
last path segment (`turn`) of its configured turn URL with the `audio_url` value. The backend
never needs to know the Peggy prefix.

#### `error.code` values (stable; new codes may be added and a node must treat unknown ones as generic)

| Code | Meaning | Brain called? |
|---|---|---|
| `empty_transcript` | STT heard nothing, or the note was under 100 ms | no |
| `transcript_too_long` | transcript is over `/ask`'s 1000-char limit. It is never clipped, because a clipped command could change meaning. | no |
| `stt_failed` | primary STT failed, and the fallback (if configured) failed too | no |
| `ask_failed` | `/ask` unreachable, timed out, non-200, or no reply text | maybe (the brain may still have run it; **do not auto-retry**, since a device could toggle twice) |
| `internal` | unexpected backend error | unknown |

A TTS failure is **not** an error. The turn still succeeds, with `audio_url: null`.

## `GET /audio/{id}.mp3`: the reply MP3

- Requires the **same bearer** as `/turn`. The firmware can send headers, unlike Twilio's
  `<Play>`, so this route is bearer-gated **and** a capability URL. The id comes from
  `secrets.token_urlsafe(18)`: 24 url-safe characters, 144 bits.
- `200 audio/mpeg`, `Cache-Control: no-store`, `X-Content-Type-Options: nosniff`,
  `Content-Length` set. The format is ElevenLabs `mp3_22050_32` (22.05 kHz mono, 32 kbps).
  The node resamples to 16 kHz in its existing decode path.
- The id stays valid for **10 minutes** (the `phone_tts.py` TTL) and can be fetched more than
  once in that window, so a retry after a dropped connection works. The store is in memory
  only, capped at 512 ids. Nothing is written to disk, and a backend restart drops every id.
- A malformed, unknown or expired id gets an identical bare `404 {"error": "not_found"}`.

## Backend pipeline (what happens inside a turn)

1. Middleware: bearer check (constant time), before the body is read.
2. Route: protocol header, `X-Node-Id`, `Content-Type`, concurrency slot, bounded and timed
   body read, WAV validation. Failures get the HTTP errors above.
3. **STT, Scribe v2 batch.** The ElevenLabs speech-to-text REST endpoint
   `POST https://api.elevenlabs.io/v1/speech-to-text` (multipart, `model_id=scribe_v2`,
   `file_format=pcm_s16le_16` with the raw PCM, `language_code=en`, `xi-api-key`) was checked
   against the ElevenLabs API reference on 2026-10-07. `scribe_v2` is the batch model id it
   lists. The node uploads a whole note, so **Scribe v2 Realtime (the WebSocket API that
   `pipeline.py` uses) is the wrong fit** and is not used. Batch Scribe v2 on a finished note
   has the same accuracy without the streaming handshake. The alternative is Deepgram Nova-3
   prerecorded (`POST https://api.deepgram.com/v1/listen?model=nova-3`, `Authorization: Token
   …`, body = canonical WAV). `VESPER_STT_PROVIDER` (`elevenlabs` | `deepgram`, default
   `elevenlabs`) picks the primary. If the primary fails, the other provider is tried, but
   **only if its key is configured**. The Studio env has no `DEEPGRAM_API_KEY` today, so
   there the fallback is unit-tested only and a failure ends as `stt_failed`.
4. **Agent turn.** `POST {VESPER_BRAIN_URL}/ask` with
   `{"text": transcript, "device_id": node_id, "channel": "node"}` and
   `Authorization: Bearer <VESPER_BRAIN_TOKEN>`. Client timeout is 30 s, above the brain's
   25 s deadline, as in `brain_llm.py`. There are no retries and no redirects. A cleartext
   brain URL is refused unless the host is loopback or tailnet. No room context is prepended
   yet (task 08 hook).
5. **TTS** (`phone_tts.py` discipline). ElevenLabs `POST /v1/text-to-speech/{voice}` with
   `model_id=eleven_flash_v2_5`, `output_format=mp3_22050_32`, voice
   `VESPER_PHONE_TTS_VOICE_ID`, a 4 s hard timeout and a 500-char cap (cut at a word boundary
   plus `...`). It uses an LRU cache (128 entries / 16 MiB) keyed by
   `sha256(voice|model|text)` and de-duplicates identical texts that are in flight.
   `VESPER_NODE_TTS=off` is the kill switch, which makes every `audio_url` null.
6. Logs carry the node id, byte and char **counts**, per-stage latencies, provider names and
   status codes. They never carry transcript or reply text, tokens, keys, or full audio ids.

## Versioning

- The current version is **1**. Every backend response carries `X-Vesper-Node-Protocol: 1`.
  A node may send the same header. If it does, the backend rejects any value other than `1`
  with `400 unsupported_protocol`. A node that omits the header is treated as v1.
- **Compatible** changes keep the version: adding SSE event types, adding JSON fields, adding
  `error.code` values. Nodes must ignore unknown events and fields.
- **Breaking** changes bump the version: changing the request body shape, renaming or
  removing events or fields, changing the auth scheme. The backend should then serve both
  versions until the fleet is OTA-updated (task 13).
- Task 09 (F1) may amend this doc while it implements the firmware side. Record any change in
  the changelog below.

## Changelog

- **v1 (2026-10-07, task 07):** initial protocol.
