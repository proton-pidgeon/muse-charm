# Task 07: Node backend service (B1)

**Goal:** A FastAPI service on the Studio (launchd) serves `POST /vesper-node/turn`: note audio → STT → `/ask` (node channel) → TTS. It is verified against a stubbed client.
**Depends on:** 06
**Relevant specs:** [`specs/01-vesper-node-architecture.md`](../specs/01-vesper-node-architecture.md), [`specs/00-conflicts.md`](../specs/00-conflicts.md) (C1)
**Source:** `docs/vesper-node-architecture.md` §2, §3, §4, §7 B1
**Est. effort:** not stated in source

## Deliverables
- [x] **Wire protocol doc first** (see conflict C1): node↔backend request/response, defaulting to §2's shape: HTTPS note upload (WAV-header + 16 kHz PCM16, base64 or raw POST) and an SSE/chunked reply carrying text deltas plus a per-message TTS MP3 URL
- [x] FastAPI service with `POST /vesper-node/turn` (route as seen by the backend after Peggy's `strip_prefix /vesper-node`; confirm the path)
> path confirmed: node-facing `/vesper-node/turn` -> backend `POST /turn` (+ `GET /audio/{id}.mp3`, open `GET /healthz`); see `docs/node-wire-protocol.md`
- [x] STT via the `pipeline.py` pattern: ElevenLabs Scribe v2 with Deepgram Nova-3 fallback via `VESPER_STT_PROVIDER`; keys from `~/.config/vesper-voice/env` (600)
- [x] Agent turn: `POST /ask {text, device_id: node_id}` + `channel="node"` with `Authorization: Bearer <VESPER_BRAIN_TOKEN>`
- [x] TTS via the `phone_tts.py` discipline: ElevenLabs `eleven_flash_v2_5`, `mp3_22050_32`, 4 s hard timeout, LRU cache + in-flight dedup, short-lived capability-URL MP3s; voice = Kevin's choice (open Q1) — resolved 2026-10-07: phone brain's `VESPER_PHONE_TTS_VOICE_ID`
- [x] Auth: re-validate the node token per route (constant-time, auth-before-body), exactly like `brain_service.py`; key in an env-style file, 600 perms
- [x] launchd unit; bind on 6PN (for the later Peggy handle)
> `backend/launchd/com.vesper.node.plist` + `make -C backend install-launchd`; default bind `::` covers the 6PN address + `[::1]` (set `VESPER_NODE_HOST` to the 6PN address for 6PN-only)
- [x] Stubbed client (script) that posts a WAV note and consumes the reply stream
- [x] Measure real-provider STT/TTS latency on the Studio and record it (documented baseline: loopback 0.66 s median; phone turn ~3–6 s)

## Definition of done
- [x] Stub client: WAV note in → transcript → `/ask` reply text → playable MP3 URL out, end to end
- [x] Missing or wrong token → 401 before the body is read
- [ ] Service survives restart under launchd
> orchestrator: verified post-merge from main checkout (mechanism proven in-branch with a throwaway `com.vesper.node.selftest` agent: kickstart -k + KeepAlive respawn, see HANDOVER 2026-10-07 ~20:47)
- [x] Latency numbers recorded in HANDOVER.md

## Anti-deliverables (do NOT build in this task)
- Agent code ("Thin shim; no agent code")
- The Peggy handle / Fly deploy (task 12, human-gated)
- The node registry and claim endpoints (task 08)
- Firmware

## Risks / unknowns
- Open Q1: the TTS voice is undecided. The Warm voice (`avocado_v2:MAI_01`) is VM-only and not callable from the Studio
- Open Q4: transcript logging is undecided. Default to transient (no transcript logs) until Kevin answers
- Open Q6: the LiveKit-client alternative is unverified. If Kevin wants the spike, it could replace this HTTP shim
- Scribe v2 *Realtime* is a streaming API, but the node uploads a whole note. Verify which ElevenLabs mode fits

## Notes for Claude Code
- Start by reading the linked `specs/` files and, if needed, the original source doc.
- Tick off boxes in this file as you complete them.
- If a deliverable is blocked, append `> blocked: <reason>` under it rather than removing it.
- Commit incrementally; one logical change per commit.
