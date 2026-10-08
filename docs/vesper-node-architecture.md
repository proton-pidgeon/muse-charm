# Vesper Node — Architecture Design

**Status:** design only (2026-10-07). No firmware or backend changes made.
**Goal (Kevin's call):** the AiPi Lite becomes a voice interface to Vesper —
his personal agent with memory, tools, and agency — not stock Meta Muse.
First node proves it; more nodes in different rooms if it works.

**v1 scope (Kevin's call 2026-10-07):** chat + home/room control through the
speaker. Architecture must be ready to grow to full Vesper later, but v1 ships
the voice-appropriate subset: conversational + smart-home first.

Repo: `proton-pidgeon/muse-charm` (public). SDK: `facebookincubator/muse-gadget-sdk`
@ `b1a3822`, cloned unmodified at `~/builds/muse-charm/scratch/muse-gadget-sdk`
on the Studio. Flash day (stock firmware) completed 2026-10-07: paired as
`MuseGadget-C86320`, first voice round trip verified.

---

## 1. How the stock firmware talks to Meta (verified in source)

All citations are from the unmodified SDK tree (`esp32/`), commit `b1a3822`.

**Voice-turn flow** (`main/voice.c`, `main/button.c`):
- Right button (GPIO42) press/release → `run_turn()`: `muse_hatch_turn_begin()`
  → `record()` → `muse_hatch_turn_end()` → `reply()` (`voice.c:224-231,143-201`).
- Capture: 16 kHz mono PCM16 (`VOICE_MIC_RATE`, `voice_board.h:31`), 20 ms
  chunks (`voice.c:51,127`), max 15 s, 250 ms release tail. Chunks feed
  `muse_hatch_turn_audio()` (`components/muse/muse_chat.h:83`).

**Wire uplink** (`components/muse/muse_chat_session.cpp`, ~2250 lines):
- TLS to `hatch.metaaivm.com:443` (NVS `muse`:`host` override),
  `GET /v1/noise?vm_id=…` + `Authorization: Bearer <vm_auth_token>` expecting
  HTTP 101 (`ws_upgrade`, :516-557). Noise handshake over WS binary frames
  (vendored `xplat/noise/core`).
- Turns go as **voice notes** (`VOICE_NOTE 1`, :119-126): `POST /chat/stream`
  with body = JSON head + base64(44-byte WAV header + 16 kHz PCM16) + tail
  (`muse_chat_priv.h:42-46`; `open_note` :1110-1130, `send_note_part` :1091-1108).
  Everything is multiplexed inside the Noise tunnel (`open_stream`, :658).

**Reply path:** NDJSON events on `POST /chat/subscribe` —
`delta.message_start` / `text_append` / `message_done` with **text payloads**
(`on_event`, :1402-1478). **Replies are text-only.** `start_tts()` (:1495-1529)
paces captions over *silence*; the TTS hook is an explicit source comment:
"send the message's text to a TTS API of your choice and play the MP3 it
returns". The MP3→16 kHz decode pipeline exists (`tts_data`, `decode`,
`muse_hatch_turn_read`), but `K_TTS` is **never opened — stock firmware cannot
speak replies**. `reply()` (`voice.c:143-201`) polls events and plays
`muse_hatch_turn_read()` PCM to the 16 kHz speaker.

> Honest note: the Oct 7 "loud and clear" round trip therefore could not have
> included spoken reply audio — it was captions + a working mic path. The
> Vesper-node firmware fills exactly this TTS slot.

**Auth chain** (`main/vm_api.c`, `main/app.c:714-730,860-935,1170-1195`,
`main/link_pairing.c`):
- Build-time SDK token (`mgst_…`) is a pairing identifier only — 401 on the VM
  API (verified by the task-02 probe 2026-10-03).
- BLE pairing via the Muse app delivers `api_url` / `api_url_v2` / `noise_host`
  + an access/refresh pair (NVS `homehub`) → `POST
  https://api.muse.ai/device_token/mint` (`vm_api.h:24`) returns the device
  token pair → `GET /fetch_vms` → per-VM `vm_auth_token` → WS-upgrade auth.
  Noise message 3 is an empty protobuf (auth already done by the Bearer token).
- **Verdict: nothing in this chain is reusable for a non-Meta backend** — every
  step mints Meta-accepted tokens from Meta endpoints. We imitate the *pattern*
  (stored pair + refresh-on-401, `app.c:860-935`) and reuse `identity_node_id()`
  (`homelink-<mac>`, `identity.c`) as our device ID.

**NVS layout:** `homehub` holds `api_url`, `api_url_v2`, `noise_host`,
`access_token`, `refresh_token`, (`auth_token` legacy), `vm_url`, `username`,
`ssid`, `password`, … — all but the Wi-Fi keys become dead weight under a
custom backend. `muse` holds `volume`, `speaker`, `mic_gain`, `bright`,
`sleep_s`, `wifi_on`, `ble_on`, `ssid`, `pass`, `host` (default
`hatch.metaaivm.com`), `vm`, `token`. Keep `muse` as-is; add our credential
key. Note: NVS is currently **plaintext** (`config_store.c:39-45`) — consider
enabling NVS encryption when we store our own tokens there.

**Task-02 lessons folded in:** task 02 (live Muse transport hack for the
simulator) was cancelled by Kevin on a misread instruction, but its code
reading stands: the `muse_hatch_*` API (`muse_chat.h`) is the intended seam
(the whole voice path touches the network only through
`turn_begin/audio/end/cancel/event/read/caption` + status), and two backends
already exist (`muse_chat_session.cpp` vs `muse_chat_link.c`, selected at
`components/muse/CMakeLists.txt:45-51`). Its blocker (needed a device token)
is moot now — we are not using Meta's transport at all.

---

## 2. The seam: recommended path (with tradeoffs)

| Option | What | Verdict |
|---|---|---|
| (a) Fork SDK, replace transport | Write our own network layer pointed at our server | Needed in part |
| (b) Proxy at network level | Keep Meta transport, MITM/proxy `hatch.metaaivm.com` | **Rejected** — would require reimplementing Meta's Noise-multiplex protocol server-side for zero benefit |
| (c) `muse_hatch_*` seam | Implement a third backend behind the existing API | **Recommended, via (a)** |

**Recommendation: fork the SDK; write a third `muse_hatch_*` backend speaking
our own simple protocol.** Concretely:

- **Our protocol (to be spec'd in the firmware task):** HTTPS note upload
  (same shape the firmware already produces: WAV-header + 16 kHz PCM16, base64
  or raw POST — simpler than Meta's), reply as SSE/chunked stream carrying text
  deltas **plus a per-message TTS MP3 URL** — filling exactly the empty TTS slot
  at `muse_chat_session.cpp:1503-1517`. The firmware's existing MP3→PCM decode
  path (`tts_data`/`decode`/`muse_hatch_turn_read`) is then actually used.
- **Drop:** `vm_api.c`, `link_pairing.c`, `noise_control*`, `ble_server.c`,
  `muse_account_api.c`, `muse_chat_session.cpp`, `CONFIG_GADGET_SDK_TOKEN`.
- **Replace pairing** (Muse-app BLE flow) with **our own claim flow** over
  `muse_ble.c`'s existing command surface (see §4).
- **Keep untouched:** `boards/board_aipi.c` (pins, 128×128 ST7789, ES8311 codec,
  buttons, GPIO10 power latch), `voice_board.c`, `voice_player.c`,
  **`main/voice.c` unchanged** (it only calls `muse_hatch_*`), the LVGL UI,
  `muse_settings.c` (repurpose `host` for our server URL), `muse_ble.c`
  (Wi-Fi provisioning commands `wifi.ssid`/`wifi.pass`/`wifi.connect`, :123-175),
  `muse_wifi`, `identity.c`, `ota.c` (OTA for multi-room updates), WAV/base64
  helpers in `muse_chat_text.c`.

Why this seam wins: `main/voice.c` never changes, so the button/LED/screen
behavior Kevin already validated stays identical; only the network backend is
swapped. The avatar work (§5) lands independently of transport work.

---

## 3. Voice pipeline: reuse vs build

The node backend is a **thin audio-transport shim** on the Studio:
`board → STT → POST /ask → TTS → board`. No new agent code.

**Reuse wholesale — the Vesper voice brain** (`~/vesper-voice` on the Studio,
launchd `com.vesper.brain`, FastAPI `server/src/vesper_voice/brain_service.py`,
built 2026-10-06, live):

- **Agent turn: `POST /ask`** — `Authorization: Bearer <VESPER_BRAIN_TOKEN>`,
  body `{"text": str, "device_id"?: str}` (device_id matches
  `[A-Za-z0-9._:-]{1,128}`), returns `{"text": spoken-first reply, "actions":
  [...], "detail"?: overflow}`. Auth is checked before the body is read
  (constant-time compare); 25 s deadline; bounded tool loop (max 6 model
  rounds, 4 tool calls/round). **Stateless per turn** — the LiveKit pipeline's
  `brain_llm.py::BrainLLM` already reuses it exactly this way (one HTTP POST
  per voice turn, `device_id: "vesper-voice-pipeline"`), as does the iOS Siri
  intent. Reachable behind Peggy at `https://peggy.fly.dev/vesper/ask`.
  Persona has channel variants (`ask`, `voice`, `phone`) in
  `persona.py::system_prompt(channel)`; spoken formatting via `ask.to_spoken()`
  (2–3 sentence caps per channel).
- **v1 fit:** `/ask` already runs the bounded tool loop with Vesper persona +
  Lobe tools (search/get/toggle/set) — i.e. **chat + home/room control are
  already the shape of an `/ask` turn**. v1 = this, plus a `channel="node"`
  variant for node-appropriate spoken formatting (small change in the
  `vesper-voice` repo, not a fork).
- **STT:** reuse the pipeline pattern — ElevenLabs Scribe v2 Realtime
  (0.14 s to final, 3.6% WER) with Deepgram Nova-3 fallback via
  `VESPER_STT_PROVIDER`; keys in `~/.config/vesper-voice/env` (600).
  (`pipeline.py`; phone path uses Twilio Gather, not applicable here.)
- **TTS:** reuse the `phone_tts.py` discipline — ElevenLabs (`eleven_flash_v2_5`,
  `mp3_22050_32`, 4 s hard timeout, LRU cache + in-flight dedup), short-lived
  capability-URL MP3s. **Voice choice is Kevin's open decision** (see §7):
  the phone uses his cloned "Decisive Hammer" voice; the WebRTC pipeline is
  provider-agnostic (Cartesia or ElevenLabs); the Warm catalog voice
  (`avocado_v2:MAI_01`) is only callable from the VM (`/opt/hatch/bin/tts`,
  verified working) — **not** from a Studio-hosted backend.

| Piece | Reuse | Build |
|---|---|---|
| Agent turn (persona, memory, tools, spoken reply) | ✅ `POST /ask` + `channel="node"` variant | nothing |
| Auth model | ✅ Caddy bearer handle + narrower token (§4) | the new handle block only |
| STT | ✅ ElevenLabs Scribe v2 / Deepgram pattern + keys | board↔STT audio transport |
| TTS | ✅ ElevenLabs pattern from `phone_tts.py` | transport + voice selection (Kevin decides) |
| Hosting/edge | ✅ Peggy static handle, 6PN bind, strip-prefix | node backend process + launchd unit |

**Latencies (documented, not re-measured):** loopback mock pipeline 0.66 s
median; phone turn ~3–6 s (Twilio end-of-speech 1–2 s + brain 1.5–4 s + TTS).
Real-provider STT/TTS latency on the Studio is unverified — measure in the
backend task.

**Alternative worth evaluating (unverified):** whether the node firmware can
act as a **LiveKit client** and ride the existing `voice-brain` worker
end-to-end. If feasible it collapses STT/agent/TTS transport into one
already-built path; if not, the HTTP shim above stands.

---

## 4. Node identity & auth

Follow the `/vesper/` bearer-token precedent exactly — **do not invent a new
scheme** (this is the pattern Kevin already approved for native clients).

**Edge (Peggy, `~/code-local/peggy/.peggy/render/Caddyfile`):**
- Add a static handle mirroring `/vesper/*` (lines 302–355): `handle
  /vesper-node/*` with matcher `@node_auth header Authorization "Bearer
  {env.VESPER_NODE_TOKEN}"` → `uri strip_prefix /vesper-node` →
  `reverse_proxy http://[6PN]:<port>` + `header_up X-Forwarded-Proto https`.
  No/wrong token → `respond "bad token" 401` **at the edge, never proxied**
  (fail-closed). Deploy via `fly deploy` from `.peggy/render/` — **needs
  Kevin's approval** (shared gateway; redeploys blip all services).
- Token from Fly secret (`fly secrets import --stage`); `≥32` chars.

**Backend:** re-validate the token per route (constant-time, auth-before-body),
exactly like `brain_service.py`. The narrower-token lesson applies: a node
token must not reach master routes — the Caddy matcher scopes it to
`/vesper-node/*` only. Key storage: `~/.config/vesper-voice/env`-style file,
600 perms.

**Claim flow (replaces Muse-app pairing):** the firmware keeps `muse_ble.c`'s
command surface for Wi-Fi provisioning (`wifi.ssid/pass/connect`) but drops
`link_pairing.c`. New flow (to be spec'd in the firmware task): node boots
unclaimed → exposes a short claim code (screen + BLE) → Kevin (or Vesper)
claims it against the backend → backend issues the node credential → stored in
NVS `muse` namespace (new key; consider enabling NVS encryption — currently
plaintext). Device ID: reuse `identity_node_id()` → `homelink-<mac>` (stable,
already unique per board).

---

## 5. Vesper avatar on the 128×128 display

**Reality check (verified in source):** the SDK avatar is **procedural pixel
art, not bitmaps**. `components/muse/avatar/muse_pixel.c` implements
`muse_pixel.h`: a **64×64 palette-indexed grid** with `render(pose)` /
`scale()` / `set_size()` / `accent(mode)`; 7 modes (BOOT / IDLE / LISTENING /
THINKING / SPEAKING / ERROR / OFF) + a happy overlay; driven per-frame by
`muse_ui.c:1482` through a custom LVGL image decoder (:225–245). On the AIPI
it is an exact **2× upscale to 128×128 RGB565**.

**Consequence:** Kevin's 1600×1600 photorealistic avatar frames
(`~/workspace/avatars/.frames/static-1791329921728084034-4-{idle,working,making_something,milestone_level_up}.webp`,
Oct 6 auditions) are **reference/inspiration, not convertible assets**. The
avatar work is **original pixel art authored in the SDK's procedural format**
— a Vesper character expressed as 64×64 palette-indexed poses — not a format
conversion.

**Spec for the avatar task:**
- Write a custom `muse_pixel.c` (build auto-detects it via the
  `components/muse/CMakeLists.txt` glob) with original art — Meta's default
  character is not rights-granted, so this must be Vesper-original anyway.
- Cover all 7 modes + happy overlay; map SDK modes to Vesper personality
  (IDLE/LISTENING/THINKING/SPEAKING are the live ones; BOOT/ERROR/OFF for
  completeness).
- Preview **without hardware** using `tools/muse/make_gifs.py`; frame budget
  <10 ms on the S3.
- **Lands independently of transport work** — can ship as its own firmware
  task and be validated on-device before the backend exists.

---

## 6. Multi-room sketch

- **Node registry (backend):** `node_id` (`homelink-<mac>`) → room
  (`"kitchen"`, `"office"`, …), per-node token or one `VESPER_NODE_TOKEN` +
  node_id claim. Room assignment is backend config (file or tiny admin
  endpoint); the node itself only knows its `node_id`.
- **`/ask` already takes `device_id`** — pass `node_id` as `device_id` and the
  agent layer can distinguish nodes today (same mechanism as
  `device_id:"vesper-voice-pipeline"`). Room context ("the kitchen node")
  enters via the `channel="node"` system-prompt variant + registry lookup in
  the backend, which can prepend room context to the turn text.
- **v1 home/room control:** Lobe tools already in the `/ask` loop
  (search/get/toggle/set). "Turn off the kitchen lights" from the kitchen node
  = registry room + Lobe neuron scoping — mostly prompt/backend work, no new
  firmware.
- **Fleet implications:** per-node firmware is identical except NVS identity;
  OTA via the kept `ota.c` for fleet updates; claim flow (§4) is per-node.
  N nodes = N rows in the registry, N claim ceremonies, one backend.

---

## 7. Work breakdown (buildable tasks)

**Firmware track** (headless Claude Code on the fleet, `/implement`, per
standing build order):
- **F1 — Fork + third `muse_hatch_*` backend.** Fork the SDK (or patch set
  tracked in this repo); delete Meta transport files (`vm_api.c`,
  `link_pairing.c`, `noise_control*`, `ble_server.c`, `muse_account_api.c`,
  `muse_chat_session.cpp`, `CONFIG_GADGET_SDK_TOKEN`); implement the backend
  speaking our protocol: HTTPS note upload (WAV-header + 16 kHz PCM16, same
  shape the firmware already produces) + SSE/chunked reply (text deltas + TTS
  MP3 URL). Keep `main/voice.c` untouched.
- **F2 — TTS playback slot.** Wire the existing MP3→16 kHz decode path
  (`tts_data`/`decode`/`muse_hatch_turn_read`) to actually open and play —
  this is the feature stock firmware lacks.
- **F3 — Claim flow.** Replace `link_pairing.c`: unclaimed boot → claim code
  on screen/BLE → backend-issued credential → NVS `muse` namespace (evaluate
  NVS encryption). Keep `muse_ble.c` Wi-Fi provisioning commands.
- **F4 — OTA + fleet hygiene.** Keep `ota.c` working against our server for
  multi-room updates.

**Backend track:**
- **B1 — Node backend service** (FastAPI, Studio, launchd): `POST
  /vesper-node/turn` — accept note audio → STT (ElevenLabs Scribe v2 pattern)
  → `POST /ask {text, device_id: node_id}` (+ `channel="node"`) → TTS reply
  (ElevenLabs pattern, Kevin's chosen voice) → return audio (capability URL
  or chunked). Thin shim; no agent code.
- **B2 — Node registry:** node_id → room mapping + credential issuance for
  the claim flow.
- **B3 — `channel="node"` variant** in `vesper-voice` (`persona.py`
  `system_prompt`, `to_spoken` caps) — small, in the existing repo.
- **B4 — Peggy static handle** `/vesper-node/*` with `VESPER_NODE_TOKEN`
  bearer matcher (Caddyfile block mirroring `/vesper/*`); Fly secret +
  deploy — **needs Kevin's explicit approval** (shared gateway).

**Avatar track (independent):**
- **A1 — Original Vesper pixel avatar** in `muse_pixel.c` format: 64×64
  palette-indexed, 7 modes + happy overlay, Vesper-original art (reference:
  `~/workspace/avatars/.frames/`). Preview with `tools/muse/make_gifs.py`.
- **A2 — On-device validation:** flash avatar-only build (stock transport
  untouched) and check all modes on the 128×128 LCD.

**Suggested order:** A1/A2 first (independent, visible win) → B3+B1+B2 (backend
against a stubbed client) → F1/F2/F3 (firmware, needs B1 live) → B4 (gateway,
human-gated) → F4 → multi-room.

---

## 8. Open product questions for Kevin

1. **TTS voice for the node.** The Warm catalog voice (`avocado_v2:MAI_01`)
   can't be called from a Studio-hosted backend (VM-local CLI only). Options:
   (a) ElevenLabs with your cloned "Decisive Hammer" voice (same as the phone
   brain); (b) pick a new dedicated Vesper voice once and use it everywhere
   (phone, node, narrated audio). Your final Vesper voice is still open in
   every track — want to decide it once, here?
2. **Push-to-talk vs wake-word.** v1 keeps the physical talk button (no
   always-on mic, no DSP work). Wake-word later? Confirm PTT is fine for v1.
3. **v1 capability allowlist.** Chat + home/room control — confirm the
   boundaries: e.g. Lobe devices (lights, vacuum, locks, thermostats) yes;
   purchases / messaging people / anything irreversible — in or out for v1?
4. **Privacy.** PTT means the mic is live only while the button is held —
   confirm that's the guarantee you want stated. Are voice notes transient
   only, or may the backend log transcripts?
5. **Room assignment.** Nodes get rooms via backend config (`kitchen`,
   `office`, …). Fine? Any rooms planned for nodes 2 and 3?
6. **LiveKit alternative.** The node *might* be able to ride the existing
   LiveKit voice-brain worker as a client instead of the HTTP shim —
   unverified. Worth a spike, or go straight for the HTTP shim?
7. **Oct 7 round trip.** "Loud and clear" — was that spoken audio or captions
   on screen? (Source says stock firmware can't speak; calibrates what the
   TTS slot needs to deliver.)

---

## Appendix: receipts

- Transport/voice flow: `main/voice.c`, `main/button.c`,
  `components/muse/muse_chat.h`, `components/muse/muse_chat_session.cpp`,
  `components/muse/muse_chat_priv.h`, `components/muse/boards/board_aipi.c`
- Auth/pairing: `main/vm_api.c`, `main/app.c`, `main/link_pairing.c`,
  `vm_api.h:24`, `components/muse/identity.c`
- Avatar: `components/muse/avatar/muse_pixel.c`, `muse_pixel.h`,
  `muse_ui.c:1482,225-245`, `tools/muse/make_gifs.py`
- NVS: `config_store.c:39-45`
- Backend: `~/vesper-voice/server/src/vesper_voice/{brain_service,ask,phone_turns,phone_tts,pipeline,brain_llm,persona}.py`,
  `~/.config/vesper-voice/env` (600), `~/code-local/peggy/.peggy/render/Caddyfile`
  (~302-355), `~/code-local/peggy/ONBOARDING.md`, `/opt/hatch/bin/tts`
- Prior art: `tasks/02-sim-live-transport.md` (seam analysis; cancelled),
  `docs/aipi-lite-bringup.md` (flash-day runbook)
