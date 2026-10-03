# Task 02: Live Muse transport for the simulator (typed chat)

**Goal:** Give the UI simulator a real conversation path: typed text turns
that talk to the actual Muse API, with replies streaming into the sim's
captions. No microphone yet — typed input only.
**Depends on:** 01 (simulator builds; scratch SDK clone at
`~/builds/muse-charm/scratch/muse-gadget-sdk`)
**Relevant docs:** [docs/simulator-setup.md](../docs/simulator-setup.md)
**Est. effort:** weekend-scale for someone comfortable in C

## Background (verified by code reading 2026-10-03)

- The sim's UI calls exactly **one** chat function: `muse_hatch_status()`
  (`components/muse/muse_ui.c:1189`), stubbed REACHABLE in
  `sim_services.c`. Space key just flips `muse_state` modes — no turn ever
  starts. The real logic lives behind the `muse_hatch_*` API in
  `components/muse/muse_chat.h`, which is the seam to implement.
- Production transport (`components/muse/muse_chat_session.cpp`, 2250
  lines): TLS to `hatch.metaaivm.com` → WS-style upgrade
  `GET <NOISE_PATH>?vm_id=...` with token → Noise protocol → then per the
  `muse_chat.h` header comment: text turns are `POST /chat/stream` with
  reply events on `POST /chat/subscribe`. Read that .cpp for the exact
  protocol — it is the spec.
- Noise is portable: `components/noise_core/` is vendored in-repo with an
  **mbedTLS backend** (`MbedtlsCryptoBackend.cpp`). No ESP32 crypto needed.
- `esp_tls` usage in the session is just connect/read/write (~15 call
  sites) — reimplement with mbedTLS directly, do NOT try to port esp_tls.
- FreeRTOS usage in the session is 23 call sites (tasks/queues/stream
  buffers) — but you do NOT need the session's task structure. Write a
  simpler host loop; only reuse the protocol framing + Noise pieces.
- Replies have somewhere to go: `muse_state_set_caption()` (public,
  `muse_state.h:62`) is what the sim already calls for scenario captions.
  Stream reply text into it; set face speaking/idle around the turn.
- Auth, tested 2026-10-03: `GET https://api.muse.ai/fetch_vms` with
  `Authorization: Bearer <token>` + `X-API-Version: 1.0.0`. The **SDK
  token (`mgst_...`) is rejected (401)** — it is a pairing identifier, not
  a credential. A **device token** (from app+BLE pairing of a real board)
  is what `fetch_vms` wants; it returns VMs with `vm_auth_token`.
  The session also has a fallback: if `fetch_vms` rejects the token but a
  VM ID is configured, the token is tried directly as that VM's token.

## Deliverables
- [ ] New sources in this repo under `sim-hack/` (our code, tracked here):
  `sim_hatch.c`/`sim_hatch.h` implementing the host side of the
  `muse_hatch_*` text-turn API:
  - `muse_hatch_text_turn(char *text)` — runs one typed turn against the
    real API (takes ownership of `text`, frees it, per the header contract)
  - `muse_hatch_turn_event(char *text, size_t cap)` — surfaces
    `MUSE_HATCH_EV_REPLY` / `DONE` / `ERROR` to the UI loop
  - keep `muse_hatch_status()` semantics (REACHABLE when a token is
    configured, NOT_SET otherwise)
- [ ] Transport inside `sim_hatch.c`: `fetch_vms` over HTTPS (mbedTLS, or
  libcurl if simpler — macOS ships it), Noise handshake via in-repo
  `components/noise_core`, then `POST /chat/stream` + subscribe per the
  session .cpp's protocol. Reuse protocol constants/framing from the .cpp;
  do not re-derive the protocol from scratch.
- [ ] Input path: typed lines on stdin (a small reader thread) feed
  `muse_hatch_text_turn`. Reply text streams into
  `muse_state_set_caption()`; set the face to speaking while streaming,
  idle when done.
- [ ] Token config: `MUSE_DEVICE_TOKEN` env var, else
  `~/.muse-charm/token` (0600). **Never committed, never logged, never
  printed** — not in code, not in HANDOVER, not in test output. With no
  token configured the sim must behave exactly as today (today's stubbed
  behavior is the fallback).
- [ ] Build: add a **separate** CMake target `muse_simulator_live` (new
  sources + `noise_core` sources + mbedTLS from Homebrew) so the pristine
  `muse_simulator` binary is untouched. The scratch SDK clone may be
  modified for the build (it's disposable), but every new source file must
  exist in this repo under `sim-hack/` first, with a note in HANDOVER.md
  of what was copied where.
- [ ] `docs/simulator-setup.md` gains a "Live mode" section: what it does,
  how to configure the token, how to run, what to expect.

## Definition of done
- [ ] `muse_simulator_live` builds clean; plain `muse_simulator` still
  builds and `ctest` still passes
- [ ] Negative test: with a dummy token, `fetch_vms` returns 401 and the
  sim reports the auth failure in-caption instead of hanging — proves the
  HTTPS path works without a real credential
- [ ] With a real device token (human supplies at runtime): typed line →
  reply streams into the caption UI; face animates speaking → idle
- [ ] No token material anywhere in the repo, logs, or screenshots

## Anti-deliverables (do NOT build in this task)
- Microphone capture / voice turns (separate task; SDL audio is compiled
  out of the sim)
- Spoken replies / TTS (same story as hardware: own TTS API)
- Committing the SDK, its build dirs, or any token
- Changing the existing `muse_simulator` target's behavior

## Risks / unknowns
- The exact `chat/stream` + subscribe event framing must be read out of
  `muse_chat_session.cpp` — it is the only spec; there is no other doc.
- If `fetch_vms` shape differs from what the .cpp parses (cJSON on
  `vm_auth_token` etc.), adapt — the .cpp is still the reference.
- Xcode 26.2 vs CLT SDK 27.0 mismatch on this Mac (task 01 notes): the
  `SDKROOT` workaround may be needed again; do not change system settings.
- No device token is available in this session — end-to-end auth stays
  human-gated. Build everything else testable.
