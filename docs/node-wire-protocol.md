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
| Q4 transcripts | **Transient only.** The backend never logs or persists transcripts or reply text. **Amended 2026-10-08 (task 16, Kevin's request):** the backend keeps a per-node conversation memory on the Studio (last 15 exchanges + a session summary, `VESPER_NODE_MEMORY_FILE`, mode 600) so follow-ups work. It is still never logged. **Amended 2026-10-08 (task 17, Kevin's approval of Part 4):** after a turn, a significant single exchange ("remember that …") may be appended, trimmed and capped, to the long-term memory candidate queue `~/memory/node-candidates.jsonl` (`VESPER_NODE_CANDIDATES_FILE`, mode 600) for Vesper to review. Never logged, never audio. | Logs carry lengths, latencies, provider, status codes only. Tests enforce this. MP3s live in memory only, with a TTL. |
| Q6 LiveKit spike | No spike. Build the HTTP shim. | This doc. |
| PTT vs wake word | PTT for v1. | Whole-note upload, below. |

## Endpoints and path mapping

The node talks to Peggy. Peggy's `/vesper-node/*` handle (task 12, human-gated) checks the
bearer at the edge, runs `uri strip_prefix /vesper-node`, and reverse-proxies to the backend
over 6PN. **The backend's own routes have no `/vesper-node` prefix.**

| Node-facing URL (via Peggy) | Backend route (after `strip_prefix`) | Auth |
|---|---|---|
| `POST https://peggy.fly.dev/vesper-node/turn` | `POST /turn` | node bearer + node credential |
| `GET https://peggy.fly.dev/vesper-node/audio/{id}.mp3` | `GET /audio/{id}.mp3` | node bearer + node credential |
| `POST https://peggy.fly.dev/vesper-node/claim/start` | `POST /claim/start` | node bearer + `X-Node-Id` |
| `POST https://peggy.fly.dev/vesper-node/claim/poll` | `POST /claim/poll` | node bearer + `X-Node-Id` + `X-Claim-Secret` |
| `POST https://peggy.fly.dev/vesper-node/admin/claim` | `POST /admin/claim` | node bearer + admin token (route off unless `VESPER_NODE_ADMIN_TOKEN` is set) |
| `GET https://peggy.fly.dev/vesper-node/firmware/manifest` | `GET /firmware/manifest` | node bearer + node credential (task 13) |
| `GET https://peggy.fly.dev/vesper-node/firmware/{sha256}.bin` | `GET /firmware/{sha256}.bin` | node bearer + node credential (task 13) |
| *(not routed by Peggy)* | `GET /healthz` | none. Returns `{"ok": true}` only, no detail. |

The task text names the route `POST /vesper-node/turn`. That is the node-facing path. The
backend serves it as `/turn` because Peggy strips the prefix. Peggy must not route
`/vesper-node/healthz` to the backend, though it would be harmless if it did.

Backend default bind: `VESPER_NODE_HOST=::`, `VESPER_NODE_PORT=8796`. `::` listens on every
interface: the 6PN address `fdaa:3e:60bd:a7b:9016:9c37:ac65:d902` (for the Peggy handle) and
IPv6 loopback `[::1]` (for the stub client and health checks). On the Studio a `::` bind is
IPv6-only, so `127.0.0.1` is **not** served. The live brain behaves the same way: its
`VESPER_BRAIN_URL` is `http://[::1]:8795`. Set `VESPER_NODE_HOST` to the 6PN address
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
- On top of the bearer, `/turn` and `/audio` need a **per-node credential** (task 08, below):
  `X-Node-Id` must be a registered node and `X-Node-Credential` must match the credential it
  was issued at claim time. This second check also runs in the middleware, before the body.

### Credential model: decision (conflict C2, task 08, 2026-10-08)

**Decision: two layers. One shared edge bearer, plus a per-node credential in its own
header.**

| Layer | Header | Checked by | Purpose |
|---|---|---|---|
| 1. Shared edge bearer | `Authorization: Bearer <VESPER_NODE_TOKEN>` | Peggy's `/vesper-node/*` matcher **and** the backend (constant time, before the body) | Keeps unauthenticated traffic off the backend. It is one static token, so it stays compatible with task 12's single `{env.VESPER_NODE_TOKEN}` matcher with no change. |
| 2. Per-node credential | `X-Node-Id: homelink-<mac>` + `X-Node-Credential: vnc_<43 chars>` | backend only, against the node registry (SHA-256 at rest, `hmac.compare_digest`, before the body) | Identifies the node and its room, and lets one node be revoked without re-keying the fleet or touching the edge. |

Why not one token per node in `Authorization`? The edge matcher can only compare against
one static token, so per-node bearers would need a new Caddy scheme. §4 says "do not invent a
new scheme". Why not the shared token plus a bare `node_id` claim? Then anyone holding the
shared token could speak as any room, and revoking one node would mean re-keying every node.

Rules that follow from the decision:

- The shared token alone can **never** approve a claim, assign or change a room, or mint a
  credential. Approval needs a separate authority: the **local CLI** run as the Studio user
  who owns the 600 registry file (`vesper-node claim …`), or the optional `POST /admin/claim`
  route with `X-Vesper-Node-Admin: <VESPER_NODE_ADMIN_TOKEN>`.
- A credential's plaintext exists in exactly one place: the single `/claim/poll` response that
  delivers it. It is minted by that poll, so it never touches disk, the approver or a log.
- The firmware (task 11) stores the credential under a new NVS `muse` key and sends it on
  every `/turn` and `/audio` request. There is no refresh token. A node whose credential stops
  working (`403 node_unauthorized`) goes back to the claim flow.
- **Transition:** a board flashed before the claim flow existed has only the shared token. An
  operator can let **one registered node** in with the shared token plus `X-Node-Id`
  (`--allow-shared-token`, see *Operator*). This is off by default, set per node, and cleared
  automatically when that node completes a claim. It cannot be switched on for a node that
  holds a credential (`nodes revoke` it first). Unregistered nodes are always refused.

## `POST /turn`: one push-to-talk turn

### Request

```
POST /vesper-node/turn HTTP/1.1
Host: peggy.fly.dev
Authorization: Bearer <VESPER_NODE_TOKEN>
X-Node-Id: homelink-<mac>
X-Node-Credential: vnc_<43 chars>    (task 08; omitted only by a node on the shared-token transition)
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
| 400 | `bad_node_id` | `X-Node-Id` missing, repeated or malformed (body never read) |
| 403 | `node_unauthorized` | node unknown, registered but unclaimed, credential wrong/repeated/revoked, or registry unreadable (body never read). The firmware should start the claim flow. |
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

- Requires the **same bearer** as `/turn`, plus the same `X-Node-Id` + `X-Node-Credential`
  (task 08). Without them it returns `400 bad_node_id` or `403 node_unauthorized`. The firmware can send headers, unlike Twilio's
  `<Play>`, so this route is bearer-gated **and** a capability URL. The id comes from
  `secrets.token_urlsafe(18)`: 24 url-safe characters, 144 bits.
- `200 audio/mpeg`, `Cache-Control: no-store`, `X-Content-Type-Options: nosniff`,
  `Content-Length` set. The format is ElevenLabs `mp3_22050_32` (22.05 kHz mono, 32 kbps).
  The node resamples to 16 kHz in its existing decode path.
- The id stays valid for **10 minutes** (the `phone_tts.py` TTL) and can be fetched more than
  once in that window, so a retry after a dropped connection works. The store is in memory
  only, capped at 512 ids and 64 MiB of clip bytes (when full, `audio_url` is null). Nothing is written to disk, and a backend restart drops every id.
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
   brain URL is refused unless the host is loopback or tailnet. **Room context (task 08):**
   `text` is `"[Vesper node in the <room>] <transcript>"`, with the room taken from the
   registry. The prefixed text must fit `/ask`'s 1000-char limit, or the turn ends as
   `transcript_too_long`. `device_id` is still the bare `node_id`.
   **Conversation memory (task 16):** after the room prefix the backend inserts
   `[Earlier session summary: …] [Recent conversation, oldest first: Kevin: … / Vesper: … // …] `
   before the transcript, newest turns nearly verbatim, older ones truncated, the whole block
   at most 640 chars and the total never over 1000. History shrinks to fit; the transcript is
   never clipped. On a node's first turn (no history) the text is exactly the task-08 form.
   The memory holds the last 15 exchanges per node; an exchange is recorded only when `/ask`
   returned a reply (a brain fallback reply such as "Sorry, …" arrives as a normal 200 and is
   recorded as a Vesper turn like any other — known, harmless). After 15 idle minutes
   (`VESPER_NODE_SESSION_IDLE_MINUTES`) the next turn first compresses the old exchanges into
   a short **extractive** summary (`Kevin asked: …; …. Vesper last said: …`, at most 300
   chars) built on the backend with no brain call: `/ask` always runs the home-control tool
   loop, so old utterances are never replayed to it. The summary is kept and the old
   exchanges are cleared.
   **Voice room assignment (task 16):** a whole utterance like "you're in the office", "this
   is the kitchen", "call this the study" or "this room is the den" never reaches the brain.
   Free-form names need a naming marker ("this room is called X", "… the X", "call this room
   Kevin's lab"); without one the name must end in a room noun, so "this room is cold" or
   "set the room to 70 degrees" go to the brain as ordinary turns.
   The backend sets the registry room and replies "Got it — this is the office now." through
   the normal `message_*` events + TTS. An unusable name gets a short spoken refusal and the
   room is unchanged. Incidental mentions ("is the office light on") go to the brain as usual.
5. **TTS** (`phone_tts.py` discipline). ElevenLabs `POST /v1/text-to-speech/{voice}` with
   `model_id=eleven_flash_v2_5`, `output_format=mp3_22050_32`, voice
   `VESPER_PHONE_TTS_VOICE_ID`, a 4 s hard timeout and a 500-char cap (cut at a word boundary
   plus `...`). It uses an LRU cache (128 entries / 16 MiB) keyed by
   `sha256(voice|model|text)` and de-duplicates identical texts that are in flight.
   `VESPER_NODE_TTS=off` is the kill switch, which makes every `audio_url` null.
6. Logs carry the node id, byte and char **counts**, per-stage latencies, provider names and
   status codes. They never carry transcript or reply text, tokens, keys, or full audio ids.

## Claim flow (task 08; firmware side is task 11)

A node with no credential (fresh flash, or after `403 node_unauthorized`) runs this flow:

```
node                                   backend                         Kevin / Vesper
 | POST /claim/start ------------------> | pending claim (10 min)          |
 | <- 200 {claim_code, claim_secret}     |                                 |
 | shows "K7M2-QX9P" on screen + BLE     |                                 |
 | POST /claim/poll (every 3 s) -------> | 202 pending                     |
 |                                       | <-- vesper-node claim K7M2-QX9P --room kitchen
 | POST /claim/poll -------------------> | mints credential, stores hash   |
 | <- 200 {credential} (exactly once)    |                                 |
 | stores it in NVS muse, then /turn with X-Node-Credential               |
```

All three node-facing routes return JSON with `Cache-Control: no-store` and errors as
`{"error": "<code>"}`. None of them reads a request body.

### `POST /claim/start`

```
POST /vesper-node/claim/start
Authorization: Bearer <VESPER_NODE_TOKEN>
X-Node-Id: homelink-<mac>
```

`200`:
```json
{"status": "pending", "claim_code": "K7M2-QX9P", "claim_secret": "vcs_<43 chars>",
 "expires_in": 600, "poll_interval": 3}
```

- `claim_code`: 8 characters from `23456789ABCDEFGHJKMNPQRSTVWXYZ` (no 0/O/1/I/L/U), shown
  as `XXXX-XXXX`. Show it on the screen and over BLE. It is **not** a secret from the node's
  point of view, but the node must not log it to serial.
- `claim_secret`: keep it **in RAM only** and send it on every poll. It ties the poll to the
  node that started the claim, so knowing the on-screen code and the shared token is not
  enough to collect someone else's credential.
- Starting again for the same `X-Node-Id` **replaces** the old claim: only the newest code
  works. A node that is already claimed may start a claim too. Its current credential keeps
  working until the new one is collected.
- Errors: `429 rate_limited` (restart within 5 s of the last start; `Retry-After: 5`),
  `429 too_many_pending` (8 claims are already pending across all nodes; `Retry-After: 60`),
  `503 registry_unavailable`.

### `POST /claim/poll`

```
POST /vesper-node/claim/poll
Authorization: Bearer <VESPER_NODE_TOKEN>
X-Node-Id: homelink-<mac>
X-Claim-Secret: vcs_<43 chars>
```

- `202 {"status": "pending", "expires_in": <s>, "poll_interval": 3}`: not approved yet. Poll
  again after `poll_interval`.
- `200 {"status": "claimed", "node_id": "...", "room": "kitchen", "credential": "vnc_<43 chars>"}`:
  approved. This is the **only** time the credential is ever sent. Store it in NVS before
  you do anything else. Only its SHA-256 stays on the backend.
- `404 {"error": "claim_not_found"}`: no claim for this node, or it expired, was replaced,
  was already delivered, or the secret is wrong. All of these look the same. After 5 wrong
  secrets the claim is dropped. Start over with `/claim/start`.
- If the `200` is lost in transit, the credential is lost too. The node starts a new claim and
  Kevin approves it again. The half-delivered credential is replaced when the new one is
  collected.

### Approving a code (claim authority; never the node)

- **Local CLI** (normal path), as the Studio user:
  `vesper-node claim K7M2-QX9P --room kitchen`. Case and the dash don't matter.
- **HTTP** (optional, for Vesper), only when `VESPER_NODE_ADMIN_TOKEN` is set (≥ 32 chars, in
  `node.env`, different from the node and brain tokens). Without it the route answers `404`.
  ```
  POST /vesper-node/admin/claim
  Authorization: Bearer <VESPER_NODE_TOKEN>        (the edge needs it)
  X-Vesper-Node-Admin: <VESPER_NODE_ADMIN_TOKEN>   (checked before the body is read)
  Content-Type: application/json

  {"code": "K7M2-QX9P", "room": "kitchen"}         (body capped at 1 KiB)
  ```
  The reply is `200 {"node_id": "...", "room": "kitchen", "replaces_access": bool}`, where
  `replaces_access` is true when the node already had a credential or shared-token access.
  Errors: `403 forbidden` (admin token missing or wrong), `400 bad_request`, `400 bad_room`,
  `404 claim_not_found`, `429 claim_locked`.
- **Brute-force bounds.** A code lives 10 minutes and is single-use. At most 8 are pending.
  After 10 wrong codes in 15 minutes (CLI and HTTP counted together), approvals lock for 15
  minutes and **every pending claim is dropped**.
- **Rooms** are free-form (open question Q5): 1-40 characters of letters, digits, space,
  `-` and `_`, starting and ending with a letter or digit. They are pasted into the brain
  prompt, so nothing else is allowed.

## Operator: node registry

- **File:** `~/.config/vesper-voice/nodes.json` (override with `VESPER_NODE_REGISTRY_FILE` in
  the environment or in `node.env`). Mode 600 in a 700 directory. A group/other-readable,
  symlinked or malformed file is refused, which is fail-closed: every node gets 403, and
  `vesper-node serve` and `check-config` exit 78. A missing file is an empty registry.
  `make check-config` prints the path, the node count and whether the admin route is on.
- **Contents:** per node, `room`, `credential_sha256`, `allow_shared_token` and `claimed_at`,
  plus pending claims (hashes only) and the failed-approval counter. Never put it in git.
- **Writes** are load-modify-write under an exclusive `flock` on `nodes.json.lock`, then temp
  file + `fsync` + atomic rename. The CLI and the running service can both write safely. The
  service reloads the file when it changes, so **CLI edits apply on the next request without a
  restart**.
- **Commands** (`uv run --locked vesper-node …` from `backend/`, or the venv's `vesper-node`):

  | Command | Effect |
  |---|---|
  | `vesper-node claim CODE --room ROOM` | approve the code a node shows |
  | `vesper-node nodes list` | nodes, rooms, access kind, pending claims (no secrets) |
  | `vesper-node nodes add ID --room ROOM [--allow-shared-token]` | pre-register a node (and optionally allow the shared-token transition) |
  | `vesper-node nodes set-room ID ROOM` | move a node |
  | `vesper-node nodes allow-shared-token ID on\|off` | toggle the transition for one node (`on` is refused once it holds a credential) |
  | `vesper-node nodes revoke ID` | drop its credential and shared-token access (it keeps its row and room) |
  | `vesper-node nodes remove ID` | delete the row |

### Transition for the live board (`homelink-c86320`, firmware before task 11)

The board on the bench has only `VESPER_NODE_TOKEN`. Once this change is deployed, the
backend refuses it (`403 node_unauthorized`) until it is registered. To keep it working until
F3 ships:

1. Deploy the new backend code (pull main into the main checkout, then `make -C backend install`).
2. **Before or right after** restarting `com.vesper.node`, run as the Studio user:
   `uv run --locked vesper-node nodes add homelink-c86320 --room <room> --allow-shared-token`
   (from `backend/` in the main checkout). There's no need to restart again, because the
   service picks up the file on the next request. If the restart comes first, the board gets
   403 only until this command runs. It is never stuck.
3. Check: `vesper-node nodes list` shows `access=SHARED-TOKEN (transition)`, and a turn from
   the board succeeds. Its `/ask` text now starts with `[Vesper node in the <room>]`, and the
   log line reads `auth=shared_token`.
4. When task 11 firmware is flashed, the board runs the claim flow and Kevin approves it. The
   shared-token allowance is cleared automatically. `nodes list` then shows
   `access=credential`.
5. If the board is lost or replaced: `vesper-node nodes revoke homelink-c86320`.

While the transition is on, anyone who holds `VESPER_NODE_TOKEN` can speak as that one node.
That is exactly the pre-task-08 status quo, limited to one node id.

## Operator: conversation memory (task 16)

- **File:** `~/.config/vesper-voice/node-memory.json` (override with `VESPER_NODE_MEMORY_FILE`),
  mode 600 in a 700 directory, written atomically by the running service only. It holds, per
  node id, the last 15 exchanges (Kevin's words + Vesper's reply), the session summary and the
  last-turn time. It never leaves the Studio.
- **Session gap:** `VESPER_NODE_SESSION_IDLE_MINUTES` (default 15).
- **Wipe:** stop the service (`launchctl bootout`), delete the file, start it again. The
  service keeps the memory in RAM, so deleting the file under a running service is undone by
  the next turn. A malformed or group-readable file is ignored (logged by name) and replaced
  on the next write.

## Operator: memory candidates (task 17)

- **File:** `~/memory/node-candidates.jsonl` (override with `VESPER_NODE_CANDIDATES_FILE`),
  append-only from the backend, mode 600 (tightened if looser), parent created 700 only if
  missing. One JSON line per candidate; schema and the review contract are in
  `docs/node-memory-integration.md`. It never leaves the Studio.
- The backend never reads curated memory and never calls the brain for this; a failed append
  is logged by label (`memory candidate not staged: … reason=…`) and never affects a turn.
- **Wipe/rotate:** take an exclusive `flock` on the file, rename or truncate it, release; the
  backend follows a rename on its next append. No service restart is needed.

## Firmware updates (task 13)

The fleet runs one firmware: every node's image is identical, and only its NVS identity
(Wi-Fi, server URL, bearer, node credential) differs. The operator publishes one image at a
time on the Studio (`vesper-node firmware publish`, see *Operator: firmware*), and every
claimed node picks it up. Both routes take the same auth as `/turn`: the shared bearer,
`X-Node-Id` and the node's `X-Node-Credential` (`401`/`400`/`403 node_unauthorized`
exactly as for `/turn`). A node also sends `X-Node-Firmware: <its version>`; the backend
only logs it (sanitised).

### `GET /firmware/manifest`

- `204` (empty body): nothing is published. The node stays on what it runs.
- `200 application/json`, `Cache-Control: no-store`:

  ```json
  {"version": "1.0.1", "sha256": "<64 lowercase hex>", "size": 2035712, "published_at": 1791500000}
  ```

  `version` is the image's own (`esp_app_desc_t.version`, from `firmware/hatch/VERSION`),
  always `MAJOR.MINOR.PATCH`. `size` is the image's exact byte count and `sha256` the SHA-256
  of all of it. Unknown fields are ignored.
- `503 firmware_unavailable`: the store on the Studio is unreadable or fails its own checks.

### `GET /firmware/{sha256}.bin`

The published image, `200 application/octet-stream` with `Content-Length`. Only the
currently published hash is served: any other name (another hash, upper case, a path, a
traversal) is `404 not_found`. The backend re-hashes the file on every request and answers
`503 firmware_unavailable` rather than send bytes that don't match. The node never takes a
URL from the manifest; it builds this one from the hash, on its configured server.

### What the node does (firmware side: `firmware/hatch/vesper_ota.c`, `main/ota.c`)

- **When:** only once claimed and online, between turns, never while resting: 10 s after
  boot, then every 6 hours plus a fixed per-node jitter of up to 30 minutes (from a hash of
  the node id, so a fleet doesn't check at once), and at once on the serial console's
  `>ota.check`. A failed check or install backs off from 1 minute, doubling, up to 6 hours,
  except while a freshly installed image is on probation: then a failed check is retried every
  20 s until one is answered.
- **Only over https:** a node whose server URL is `http://` keeps working for turns but skips
  update checks (`>status` shows `"update":"needs_https"`). The images are signed, but with
  the SDK's public development key, so over plain http a path attacker could swap the
  manifest and the image together.
- **What it installs:** only a version strictly newer than the one it runs (never equal or
  older), never a version that was already rolled back on this node, never one larger than its
  update slot (4 MiB on the AIPI).
- **How:** the SDK's `ota.c` (`ota_start_request`) streams the image into the other app slot
  with the same headers and no redirects. It refuses the image unless its descriptor carries
  exactly the manifest's version, exactly `size` bytes arrive, and the bytes written to the
  slot hash to `sha256`. Only then does `esp_https_ota_finish()` verify the image itself (its
  SHA-256 and RSA signature) and switch the boot partition. A turn in progress finishes before
  the restart.
- **Keeping it:** the new image boots `PENDING_VERIFY`. `app.c` marks it valid only once Wi-Fi
  is up **and** this server has answered an update check made with the node's credential
  (`200` or `204`) within 300 s. Otherwise the bootloader rolls back to the previous image,
  which then never installs that version again. So an image that can't reach the server, or a
  node that came back without its credential, is rolled back automatically. Nothing is
  installed while the running image is itself still `PENDING_VERIFY`.

## Operator: firmware

```sh
cd ~/builds/muse-charm/muse-charm/backend
uv run --locked vesper-node firmware publish <SDK>/esp32/build-muse-aipi/muse-gadget.bin
uv run --locked vesper-node firmware status
uv run --locked vesper-node firmware withdraw     # stop a rollout: nodes see 204
```

The store is `~/.config/vesper-voice/firmware/` (mode 700, override
`VESPER_NODE_FIRMWARE_DIR`). `publish` refuses anything that isn't an ESP32-S3 app image of the
`muse-gadget` project with a `MAJOR.MINOR.PATCH` version, anything over 4 MiB, and (without
`--force`) a version not newer than the one published. The running service sees a publish on
its next request; no restart.

## Versioning

- The current version is **1**. Every backend response carries `X-Vesper-Node-Protocol: 1`.
  A node may send the same header. If it does, the backend rejects any value other than `1`
  with `400 unsupported_protocol`. A node that omits the header is treated as v1.
- **Compatible** changes keep the version: adding SSE event types, adding JSON fields, adding
  `error.code` values, adding routes. Nodes must ignore unknown events and fields.
- Task 08's per-node credential **kept version 1** even though it touches auth. The request and
  reply shapes don't change, and the extra header is negotiated per node by the registry, not
  by the protocol version. v1 firmware without the header keeps working through the
  per-node shared-token transition.
- **Breaking** changes bump the version: changing the request body shape, renaming or
  removing events or fields, changing the auth scheme. The backend should then serve both
  versions until the fleet is OTA-updated (task 13, *Firmware updates*).
- Task 09 (F1) may amend this doc while it implements the firmware side. Record any change in
  the changelog below.

## Node firmware notes (tasks 09-11)

How the firmware backend (`firmware/hatch/`, the third `muse_hatch_*` backend) uses v1. None
of this changes the wire shape, so the version stays 1.

- **Upload.** The node opens `POST <base>/turn` at the button press with
  `Transfer-Encoding: chunked`, sends the 44-byte streaming WAV header (both sizes
  `0xFFFFFFFF`), then streams the PCM in chunks of about 4 KB (128 ms) while the button is
  held, so the TLS handshake and most of the upload overlap the speech. It sends all five
  request headers on every turn, `X-Vesper-Node-Protocol: 1` included. It keeps a copy of the
  note (at most 512 KiB) only for the one `503 busy` retry, which re-sends it with a
  `Content-Length`. A note under 0.3 s is never sent (`DIDN'T CATCH THAT` on the device).
- **Base URL.** NVS `muse`:`host` holds the base URL, e.g. `https://peggy.fly.dev/vesper-node`
  or `http://[::1]:8796`. The turn URL is `<base>/turn`. `http://` is accepted so a LAN, `[::1]`
  or 6PN URL works before the Peggy handle (task 12) exists. `https://` uses the ESP-IDF
  certificate bundle. Redirects are never followed, so the bearer can't be carried elsewhere.
  The URL is at most 63 characters (`MUSE_HOST_MAX`).
- **`audio_url` is accepted only as a plain relative reference.** Allowed are `audio/<id>.mp3`
  and `/audio/<id>.mp3`, built from `[A-Za-z0-9._~-]` segments. The node refuses anything with a
  scheme, `//`, `.`/`..` segments, `%`, `?`, `#`, `\`, whitespace, or more than 160 characters,
  and treats it as `null` (caption over silence). It sends its bearer to that URL, so it must
  never point off the configured server. The backend's `audio/<token_urlsafe>.mp3` passes.
- **SSE limits.** Comment lines of any length are skipped without being buffered, so the 2 KB
  preamble and pings cost nothing. A single field line, or one event's joined `data`, longer
  than 4 KiB drops that event. **Keep every event's `data` under 4 KiB.** A v1 `text_delta`
  carrying a whole spoken reply (a few sentences) is far below that. Each caption message
  keeps at most 1023 bytes of text, cut on a UTF-8 character boundary, and at most 4 messages
  per turn are shown. CRLF, LF and CR line endings are all accepted. Unknown events and fields
  are ignored. A `done` with `ok: true` after an `error` still ends the turn as failed.
  Nothing after `done` is read.
- **Timeouts.** Connect and each upload write: 10 s. Release to status: 15 s. During the reply,
  60 s with no bytes at all (pings count) ends the turn with `VESPER TIMED OUT`. The whole turn
  is capped at 180 s. A stream that ends without `done` still shows any text that arrived.
  With no text it fails as `LOST CONNECTION TO VESPER`.
- **Captions for pre-stream errors** (`vp_http_verdict`): 401 `TOKEN REFUSED`, 403
  `NODE NOT CLAIMED` (`node_unauthorized`, task 11; the node then claims again), 400
  `BAD DEVICE ID` / `UPDATE THE FIRMWARE` / `REQUEST REFUSED`, 413 `NOTE TOO LONG`, 415
  `BAD AUDIO TYPE`, 422 `COULDN'T READ THE AUDIO`, 408 `UPLOAD TOO SLOW`, 404/405
  `CHECK THE SERVER URL`, 503 (after the retry) `VESPER IS BUSY`, no response
  `CAN'T REACH VESPER`, anything else `VESPER SERVER ERROR`. For an SSE `error` the node shows
  the server's `message`. If that is empty, it falls back to a fixed caption per `code`.
- **Reachability check.** The settings "test" action (serial `>hatch.test`) sends
  `GET <base>/healthz` with the bearer. 200 means connected and 401 means the token was refused.
  Any other status counts as reachable. Through Peggy the edge needs the bearer, which is why
  the node sends it even though the backend's `/healthz` ignores it.
- **TTS (task 10).** On `message_done` with a non-null, accepted `audio_url`, the firmware
  records the resolved URL (`vesper_tts_slot_offer`). When the message's turn to be said comes, it
  sends `GET <url>` with the same `Authorization` and `X-Node-Id`, plus `Accept: audio/mpeg`.
  It never follows redirects and allows 5 s for connect and headers. It needs `200` with
  `audio/mpeg` (or no Content-Type) and at most 2 MiB. It reads `Content-Length` when it is
  present; chunked is also accepted. The node decodes the MP3 as it arrives, resamples it to
  16 kHz and plays it, with the caption timed by the audio. It falls back to the stock silent
  caption pacing (16 chars/s) when:
  - `audio_url` is null or refused;
  - the GET fails or returns anything else;
  - no frame decodes.

  A download cut short or stalled for 10 s plays what arrived and paces the rest of the caption.
  A `404` (expired id) is a normal fallback; the node doesn't retry. The node never logs the
  URL, only status, byte counts and timings.
- **Claim flow (task 11).** The node side of *Claim flow* above, in `firmware/hatch/vesper_claim.c`
  (pure C, host-tested) and `muse_chat_vesper.c`:
  - **When.** A node with no credential in NVS `muse:node_cred` (fresh flash, setup reset,
    serial `>claim.forget`) claims once the server URL and bearer are set and Wi-Fi is up. A
    `403 node_unauthorized` on `/turn` or on an `/audio` GET also sends a node back to the claim flow. Its old
    credential stays in NVS until the new one replaces it. Until it is claimed, the node
    starts no turns. On the AIPI a press still records the note, which is sent once the node is
    claimed.
  - **Requests.** `/claim/start` and `/claim/poll` carry `Authorization`, `X-Node-Id`,
    `X-Vesper-Node-Protocol: 1` and `Accept: application/json`. The poll adds `X-Claim-Secret`.
    There is no body (`Content-Length: 0`), and redirects are never followed.
  - **Accepted values.** The node takes `claim_code` only as exactly `XXXX-XXXX` from the claim
    alphabet. It takes `claim_secret` and `credential` only as `vcs_`/`vnc_` plus 16-96
    url-safe base64 characters, and `room` only as `ROOM_RE`. Anything else counts as a server
    error, and nothing of it is kept. A credential that fails these checks is lost, since it is
    delivered once, so the node starts over.
  - **Timing.**
    - It polls every 3 s and doesn't read `poll_interval`.
    - A `404` on a poll means start over, no sooner than 5.5 s after the last start. That
      respects the backend's 5 s restart limit.
    - `429` waits for `Retry-After`, capped at 900 s.
    - `503` waits for `Retry-After` if one is given, else backs off.
    - No response or a 5xx backs off from 5 s, doubling up to 60 s. Polling keeps the secret
      meanwhile.
    - `401`, `400`, or `404`/`405` on start (wrong URL, or a pre-task-08 backend) waits 60 s.
  - **Display.** The code is shown on the screen (idle caption and settings status) and in the
    BLE claim characteristic (service `76657370-6572-4e6f-6465-000000000001`, READ/NOTIFY JSON
    `{"state":"pending","code":"…"}`). It is never written to serial. The secret lives in RAM
    only and is wiped when the claim ends.
  - **After the claim.** Every `/turn` and every `/audio` GET carries
    `X-Node-Credential: vnc_…`. There is no refresh: the stock firmware's refresh-on-401
    (`app.c:860-935`) has no counterpart in this credential model. Its equivalent is re-claim on
    `403 node_unauthorized`. A `401` on `/turn` is still `TOKEN REFUSED`.
  - **Captions.** `vp_http_verdict` gives `403 node_unauthorized` the caption
    `NODE NOT CLAIMED`, and any other `403` the caption `REQUEST REFUSED`.
  - **NVS.** It is plaintext on the dev boards, by decision: see `firmware/README.md`, *NVS
    encryption: decision*. A stolen credential is handled with `vesper-node nodes revoke`.

## Changelog

- **v1 (2026-10-07, task 07):** initial protocol.
- **v1, firmware notes (2026-10-07, task 09):** added *Node firmware notes*, which describes how
  the node uses v1: chunked upload, `audio_url` acceptance rules, the 4 KiB SSE event limit,
  timeouts and captions. These are clarifications only, with no wire change and no version bump.
- **v1, node registry (2026-10-08, task 08):** conflict C2 decided (shared edge bearer +
  per-node `X-Node-Credential`). Added `403 node_unauthorized` on `/turn` and `/audio`, the
  claim routes (`/claim/start`, `/claim/poll`, `/admin/claim`), room context in the `/ask`
  text, and the *Operator* section with the shared-token transition. The version stays 1
  (see *Versioning*).
- **v1, firmware notes (2026-10-08, task 10):** the TTS bullet now describes how the node fetches
  and plays the MP3. This is a clarification only: no wire change and no version bump.
- **v1, firmware notes (2026-10-08, task 11):** added the *Claim flow (task 11)* bullet: how
  the node runs the claim flow, which response values it accepts, its poll and backoff timing,
  where the code is shown, and re-claim on `403` (no refresh). These are clarifications only:
  task 08 defined the wire shape, so there is no version bump.
- **v1, firmware updates (2026-10-08, task 13):** added `GET /firmware/manifest` and
  `GET /firmware/{sha256}.bin` (same node auth as `/turn`), the `X-Node-Firmware` header, and
  the *Firmware updates* and *Operator: firmware* sections. New routes only, so the version
  stays 1 (see *Versioning*).
- **v1, conversation memory (2026-10-08, task 16):** Q4 amended (per-node conversation memory
  on the Studio), history + session summary in the `/ask` text, voice room assignment. No wire
  change: the node sees the same events, so the version stays 1.
- **v1, memory candidates (2026-10-08, task 17):** Q4 amended again (single-exchange memory
  candidates staged after the turn). No wire change, so the version stays 1.
