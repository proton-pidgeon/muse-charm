# Vesper Node — Architecture Design

**Source:** `docs/vesper-node-architecture.md`
**Kind:** overview / architecture (with an embedded implementation plan in §7)
**Status:** Derived spec

## Summary
Turns the AiPi Lite (ESP32-S3, flashed with Meta's `muse-gadget-sdk` @ `b1a3822`) into a push-to-talk voice interface to Vesper instead of stock Meta Muse. The firmware keeps `main/voice.c` and swaps only the network backend behind the existing `muse_hatch_*` seam. That backend talks to a thin Studio-hosted shim: board → STT → `POST /ask` → TTS → board. v1 scope is chat + home/room control. The architecture must be able to grow to full Vesper and to multiple rooms.

## Key requirements / contracts

### Scope (source header)
- "the AiPi Lite becomes a voice interface to Vesper — his personal agent with memory, tools, and agency — not stock Meta Muse. First node proves it; more nodes in different rooms if it works."
- "**v1 scope (Kevin's call 2026-10-07):** chat + home/room control through the speaker. Architecture must be ready to grow to full Vesper later, but v1 ships the voice-appropriate subset: conversational + smart-home first."
- SDK: `facebookincubator/muse-gadget-sdk` @ `b1a3822`, cloned unmodified at `~/builds/muse-charm/scratch/muse-gadget-sdk` on the Studio. Flash day (stock firmware) completed 2026-10-07: paired as `MuseGadget-C86320`.

### §1 Stock firmware facts (verified in source, `esp32/` @ `b1a3822`)
- **Voice turn:** Right button (GPIO42) press/release → `run_turn()`: `muse_hatch_turn_begin()` → `record()` → `muse_hatch_turn_end()` → `reply()` (`voice.c:224-231,143-201`).
- **Capture:** 16 kHz mono PCM16 (`VOICE_MIC_RATE`, `voice_board.h:31`), 20 ms chunks (`voice.c:51,127`), max 15 s, 250 ms release tail. Chunks feed `muse_hatch_turn_audio()` (`components/muse/muse_chat.h:83`).
- **Meta uplink:** TLS to `hatch.metaaivm.com:443` (NVS `muse`:`host` override), `GET /v1/noise?vm_id=…` + `Authorization: Bearer <vm_auth_token>` expecting HTTP 101; Noise handshake over WS binary frames. Turns are voice notes: `POST /chat/stream` with body = JSON head + base64(44-byte WAV header + 16 kHz PCM16) + tail (`muse_chat_priv.h:42-46`; `open_note` :1110-1130, `send_note_part` :1091-1108).
- **Reply path:** NDJSON events on `POST /chat/subscribe`: `delta.message_start` / `text_append` / `message_done` with text payloads (`on_event`, :1402-1478). "**Replies are text-only.**" `start_tts()` (:1495-1529) paces captions over silence; the source comment for the TTS hook reads: "send the message's text to a TTS API of your choice and play the MP3 it returns". The MP3→16 kHz decode pipeline exists (`tts_data`, `decode`, `muse_hatch_turn_read`), but "`K_TTS` is **never opened — stock firmware cannot speak replies**."
- **Auth chain verdict:** "**nothing in this chain is reusable for a non-Meta backend**". Imitate the *pattern* (stored pair + refresh-on-401, `app.c:860-935`) and reuse `identity_node_id()` (`homelink-<mac>`, `identity.c`) as the device ID.
- **NVS:** `homehub` keys (`api_url`, `api_url_v2`, `noise_host`, `access_token`, `refresh_token`, `auth_token` legacy, `vm_url`, `username`, `ssid`, `password`, …) are dead weight under a custom backend, except the Wi-Fi keys. `muse` holds `volume`, `speaker`, `mic_gain`, `bright`, `sleep_s`, `wifi_on`, `ble_on`, `ssid`, `pass`, `host` (default `hatch.metaaivm.com`), `vm`, `token`. "Keep `muse` as-is; add our credential key." NVS is currently **plaintext** (`config_store.c:39-45`): "consider enabling NVS encryption when we store our own tokens there."
- **Seam:** the `muse_hatch_*` API (`muse_chat.h`) is the intended seam. The voice path touches the network only through `turn_begin/audio/end/cancel/event/read/caption` + status. Two backends already exist (`muse_chat_session.cpp` vs `muse_chat_link.c`), selected at `components/muse/CMakeLists.txt:45-51`.

### §2 The seam (recommended path)
| Option | What | Verdict |
|---|---|---|
| (a) Fork SDK, replace transport | Write our own network layer pointed at our server | Needed in part |
| (b) Proxy at network level | Keep Meta transport, MITM/proxy `hatch.metaaivm.com` | **Rejected** — would require reimplementing Meta's Noise-multiplex protocol server-side for zero benefit |
| (c) `muse_hatch_*` seam | Implement a third backend behind the existing API | **Recommended, via (a)** |

- "**Recommendation: fork the SDK; write a third `muse_hatch_*` backend speaking our own simple protocol.**"
- **Drop:** `vm_api.c`, `link_pairing.c`, `noise_control*`, `ble_server.c`, `muse_account_api.c`, `muse_chat_session.cpp`, `CONFIG_GADGET_SDK_TOKEN`.
- **Replace pairing** (Muse-app BLE flow) with our own claim flow over `muse_ble.c`'s existing command surface (§4).
- **Keep untouched:** `boards/board_aipi.c` (pins, 128×128 ST7789, ES8311 codec, buttons, GPIO10 power latch), `voice_board.c`, `voice_player.c`, **`main/voice.c` unchanged**, the LVGL UI, `muse_settings.c` (repurpose `host` for our server URL), `muse_ble.c` (Wi-Fi provisioning commands `wifi.ssid`/`wifi.pass`/`wifi.connect`, :123-175), `muse_wifi`, `identity.c`, `ota.c`, WAV/base64 helpers in `muse_chat_text.c`.
- Rationale: "`main/voice.c` never changes, so the button/LED/screen behavior Kevin already validated stays identical; only the network backend is swapped."

### §3 Voice pipeline: reuse vs build
- Node backend = "a **thin audio-transport shim** on the Studio: `board → STT → POST /ask → TTS → board`. No new agent code."
- Reuse the Vesper voice brain (`~/vesper-voice` on the Studio, launchd `com.vesper.brain`, FastAPI `server/src/vesper_voice/brain_service.py`).
- v1 fit: `/ask` already runs the bounded tool loop with Vesper persona + Lobe tools (search/get/toggle/set). v1 = that, plus a `channel="node"` variant for node-appropriate spoken formatting ("small change in the `vesper-voice` repo, not a fork").
- **STT:** reuse the pipeline pattern: ElevenLabs Scribe v2 Realtime (0.14 s to final, 3.6% WER) with Deepgram Nova-3 fallback via `VESPER_STT_PROVIDER`; keys in `~/.config/vesper-voice/env` (600) (`pipeline.py`).
- **TTS:** reuse the `phone_tts.py` discipline: ElevenLabs (`eleven_flash_v2_5`, `mp3_22050_32`, 4 s hard timeout, LRU cache + in-flight dedup), short-lived capability-URL MP3s. The voice choice is Kevin's open decision. The Warm catalog voice (`avocado_v2:MAI_01`) is only callable from the VM (`/opt/hatch/bin/tts`), **not** from a Studio-hosted backend.
- **Latencies (documented, not re-measured):** loopback mock pipeline 0.66 s median; phone turn ~3–6 s. "Real-provider STT/TTS latency on the Studio is unverified — measure in the backend task."

| Piece | Reuse | Build |
|---|---|---|
| Agent turn (persona, memory, tools, spoken reply) | ✅ `POST /ask` + `channel="node"` variant | nothing |
| Auth model | ✅ Caddy bearer handle + narrower token (§4) | the new handle block only |
| STT | ✅ ElevenLabs Scribe v2 / Deepgram pattern + keys | board↔STT audio transport |
| TTS | ✅ ElevenLabs pattern from `phone_tts.py` | transport + voice selection (Kevin decides) |
| Hosting/edge | ✅ Peggy static handle, 6PN bind, strip-prefix | node backend process + launchd unit |

### §4 Node identity & auth
- "Follow the `/vesper/` bearer-token precedent exactly — **do not invent a new scheme**."
- **Edge (Peggy, `~/code-local/peggy/.peggy/render/Caddyfile`):** add a static handle mirroring `/vesper/*` (lines 302–355) — `handle /vesper-node/*` with matcher `@node_auth header Authorization "Bearer {env.VESPER_NODE_TOKEN}"` → `uri strip_prefix /vesper-node` → `reverse_proxy http://[6PN]:<port>` + `header_up X-Forwarded-Proto https`. No/wrong token → `respond "bad token" 401` **at the edge, never proxied** (fail-closed). Deploy via `fly deploy` from `.peggy/render/`. This **needs Kevin's approval** (shared gateway; redeploys blip all services). Token from Fly secret (`fly secrets import --stage`); `≥32` chars.
- **Backend:** "re-validate the token per route (constant-time, auth-before-body), exactly like `brain_service.py`." "a node token must not reach master routes — the Caddy matcher scopes it to `/vesper-node/*` only." Key storage: `~/.config/vesper-voice/env`-style file, 600 perms.
- **Claim flow:** keep `muse_ble.c` Wi-Fi provisioning (`wifi.ssid/pass/connect`) and drop `link_pairing.c`. "node boots unclaimed → exposes a short claim code (screen + BLE) → Kevin (or Vesper) claims it against the backend → backend issues the node credential → stored in NVS `muse` namespace (new key; consider enabling NVS encryption — currently plaintext)." Device ID: `identity_node_id()` → `homelink-<mac>`.

### §5 Vesper avatar on the 128×128 display
- The SDK avatar is **procedural pixel art, not bitmaps**: `components/muse/avatar/muse_pixel.c` implements `muse_pixel.h`, a **64×64 palette-indexed grid** with `render(pose)` / `scale()` / `set_size()` / `accent(mode)`. It has 7 modes (BOOT / IDLE / LISTENING / THINKING / SPEAKING / ERROR / OFF) plus a happy overlay, driven per-frame by `muse_ui.c:1482` through a custom LVGL image decoder (:225–245). On the AIPI it is an exact **2× upscale to 128×128 RGB565**.
- Kevin's 1600×1600 photorealistic frames (`~/workspace/avatars/.frames/static-1791329921728084034-4-{idle,working,making_something,milestone_level_up}.webp`) are "**reference/inspiration, not convertible assets**."
- Spec for the avatar task (verbatim):
  - "Write a custom `muse_pixel.c` (build auto-detects it via the `components/muse/CMakeLists.txt` glob) with original art — Meta's default character is not rights-granted, so this must be Vesper-original anyway."
  - "Cover all 7 modes + happy overlay; map SDK modes to Vesper personality (IDLE/LISTENING/THINKING/SPEAKING are the live ones; BOOT/ERROR/OFF for completeness)."
  - "Preview **without hardware** using `tools/muse/make_gifs.py`; frame budget <10 ms on the S3."
  - "**Lands independently of transport work** — can ship as its own firmware task and be validated on-device before the backend exists."

### §6 Multi-room sketch
- **Node registry (backend):** `node_id` (`homelink-<mac>`) → room (`"kitchen"`, `"office"`, …), "per-node token or one `VESPER_NODE_TOKEN` + node_id claim." Room assignment is backend config (file or tiny admin endpoint); the node only knows its `node_id`.
- Pass `node_id` as `/ask` `device_id`. Room context enters via the `channel="node"` system-prompt variant + registry lookup in the backend, "which can prepend room context to the turn text."
- "Turn off the kitchen lights" from the kitchen node = registry room + Lobe neuron scoping — "mostly prompt/backend work, no new firmware."
- Fleet: per-node firmware is identical except NVS identity; OTA via kept `ota.c`; claim flow is per-node. "N nodes = N rows in the registry, N claim ceremonies, one backend."

### §7 Work breakdown (verbatim)
**Firmware track** (headless Claude Code on the fleet, `/implement`, per standing build order):
- **F1 — Fork + third `muse_hatch_*` backend.** Fork the SDK (or patch set tracked in this repo); delete Meta transport files (`vm_api.c`, `link_pairing.c`, `noise_control*`, `ble_server.c`, `muse_account_api.c`, `muse_chat_session.cpp`, `CONFIG_GADGET_SDK_TOKEN`); implement the backend speaking our protocol: HTTPS note upload (WAV-header + 16 kHz PCM16, same shape the firmware already produces) + SSE/chunked reply (text deltas + TTS MP3 URL). Keep `main/voice.c` untouched.
- **F2 — TTS playback slot.** Wire the existing MP3→16 kHz decode path (`tts_data`/`decode`/`muse_hatch_turn_read`) to actually open and play — this is the feature stock firmware lacks.
- **F3 — Claim flow.** Replace `link_pairing.c`: unclaimed boot → claim code on screen/BLE → backend-issued credential → NVS `muse` namespace (evaluate NVS encryption). Keep `muse_ble.c` Wi-Fi provisioning commands.
- **F4 — OTA + fleet hygiene.** Keep `ota.c` working against our server for multi-room updates.

**Backend track:**
- **B1 — Node backend service** (FastAPI, Studio, launchd): `POST /vesper-node/turn` — accept note audio → STT (ElevenLabs Scribe v2 pattern) → `POST /ask {text, device_id: node_id}` (+ `channel="node"`) → TTS reply (ElevenLabs pattern, Kevin's chosen voice) → return audio (capability URL or chunked). Thin shim; no agent code.
- **B2 — Node registry:** node_id → room mapping + credential issuance for the claim flow.
- **B3 — `channel="node"` variant** in `vesper-voice` (`persona.py` `system_prompt`, `to_spoken` caps) — small, in the existing repo.
- **B4 — Peggy static handle** `/vesper-node/*` with `VESPER_NODE_TOKEN` bearer matcher (Caddyfile block mirroring `/vesper/*`); Fly secret + deploy — **needs Kevin's explicit approval** (shared gateway).

**Avatar track (independent):**
- **A1 — Original Vesper pixel avatar** in `muse_pixel.c` format: 64×64 palette-indexed, 7 modes + happy overlay, Vesper-original art (reference: `~/workspace/avatars/.frames/`). Preview with `tools/muse/make_gifs.py`.
- **A2 — On-device validation:** flash avatar-only build (stock transport untouched) and check all modes on the 128×128 LCD.

**Suggested order:** A1/A2 first (independent, visible win) → B3+B1+B2 (backend against a stubbed client) → F1/F2/F3 (firmware, needs B1 live) → B4 (gateway, human-gated) → F4 → multi-room.

## Schemas / interfaces

**Existing brain contract — `POST /ask`** (verbatim from §3):
> `Authorization: Bearer <VESPER_BRAIN_TOKEN>`, body `{"text": str, "device_id"?: str}` (device_id matches `[A-Za-z0-9._:-]{1,128}`), returns `{"text": spoken-first reply, "actions": [...], "detail"?: overflow}`. Auth is checked before the body is read (constant-time compare); 25 s deadline; bounded tool loop (max 6 model rounds, 4 tool calls/round). **Stateless per turn** — the LiveKit pipeline's `brain_llm.py::BrainLLM` already reuses it exactly this way (one HTTP POST per voice turn, `device_id: "vesper-voice-pipeline"`), as does the iOS Siri intent. Reachable behind Peggy at `https://peggy.fly.dev/vesper/ask`. Persona has channel variants (`ask`, `voice`, `phone`) in `persona.py::system_prompt(channel)`; spoken formatting via `ask.to_spoken()` (2–3 sentence caps per channel).

**New node endpoint (B1):** `POST /vesper-node/turn`. Only the shape is defined in the source: "accept note audio → STT → `POST /ask {text, device_id: node_id}` (+ `channel="node"`) → TTS reply → return audio (capability URL or chunked)."

**Node ↔ backend wire protocol (§2, "to be spec'd in the firmware task"):**
> HTTPS note upload (same shape the firmware already produces: WAV-header + 16 kHz PCM16, base64 or raw POST — simpler than Meta's), reply as SSE/chunked stream carrying text deltas **plus a per-message TTS MP3 URL** — filling exactly the empty TTS slot at `muse_chat_session.cpp:1503-1517`.

**Firmware seam — `muse_hatch_*` (`components/muse/muse_chat.h`):** `turn_begin/audio/end/cancel/event/read/caption` + status. `muse_hatch_turn_audio()` at `muse_chat.h:83`.

**Peggy Caddy handle (§4):**
```
handle /vesper-node/*
  @node_auth header Authorization "Bearer {env.VESPER_NODE_TOKEN}"
  → uri strip_prefix /vesper-node
  → reverse_proxy http://[6PN]:<port>  + header_up X-Forwarded-Proto https
  no/wrong token → respond "bad token" 401   (at the edge, never proxied)
```
(This block is assembled from the §4 prose. It is not literal Caddyfile syntax; mirror lines 302–355 of the real Caddyfile.)

**Device identity:** `identity_node_id()` → `homelink-<mac>`.

**Avatar API (`muse_pixel.h`):** `render(pose)` / `scale()` / `set_size()` / `accent(mode)`. Modes: `BOOT / IDLE / LISTENING / THINKING / SPEAKING / ERROR / OFF` + happy overlay. 64×64 palette-indexed → 2× → 128×128 RGB565.

## Open questions
From §8 ("Open product questions for Kevin"), verbatim headings:
1. **TTS voice for the node.** (a) ElevenLabs with the cloned "Decisive Hammer" voice, or (b) pick a new dedicated Vesper voice once and use it everywhere. The Warm voice (`avocado_v2:MAI_01`) is not callable from the Studio.
2. **Push-to-talk vs wake-word.** v1 keeps the physical talk button. Wake-word later? Confirm PTT is fine for v1.
3. **v1 capability allowlist.** Lobe devices (lights, vacuum, locks, thermostats) yes; "purchases / messaging people / anything irreversible — in or out for v1?"
4. **Privacy.** Confirm the "mic live only while the button is held" guarantee. "Are voice notes transient only, or may the backend log transcripts?"
5. **Room assignment.** Rooms via backend config: fine? Rooms for nodes 2 and 3?
6. **LiveKit alternative.** Could the node ride the existing LiveKit `voice-brain` worker as a client instead of the HTTP shim? Unverified. Spike, or go straight to the HTTP shim?
7. **Oct 7 round trip.** Was "loud and clear" spoken audio or on-screen captions? The source says stock firmware can't speak.

Other items the source leaves undecided:
- The exact node↔backend wire protocol: "to be spec'd in the firmware task" (§2).
- The exact claim flow: "to be spec'd in the firmware task" (§4).
- Whether to fork the SDK or keep a "patch set tracked in this repo" (F1).
- Whether to enable NVS encryption ("consider" / "evaluate").
- Credential model: "per-node token or one `VESPER_NODE_TOKEN` + node_id claim" (§6).
- Real-provider STT/TTS latency on the Studio is unverified.

See also `specs/00-conflicts.md`.

## Out of scope
- Option (b), proxying/MITM of Meta's transport, is rejected.
- Any reuse of Meta's auth chain or tokens.
- New agent code in the node backend ("Thin shim; no agent code"). It also says "Agent turn … Build: nothing".
- Changes to `main/voice.c`, `board_aipi.c`, `voice_board.c`, `voice_player.c`, the LVGL UI.
- Converting the photorealistic avatar frames (reference only).
- v1 excludes always-on mic / wake-word ("no always-on mic, no DSP work").
- The doc itself states: "design only … No firmware or backend changes made."

## See also
- Original: `docs/vesper-node-architecture.md`
- Related: `docs/aipi-lite-bringup.md` (flash-day runbook), `tasks/02-sim-live-transport.md` (seam analysis; cancelled), `specs/00-conflicts.md`
