# Task 09: SDK fork + third `muse_hatch_*` backend (F1)

**Goal:** The AiPi firmware runs a third `muse_hatch_*` backend that speaks our protocol to the node backend, with Meta's transport removed and `main/voice.c` unchanged.
**Depends on:** 07 (B1 live), protocol doc from 07
**Relevant specs:** [`specs/01-vesper-node-architecture.md`](../specs/01-vesper-node-architecture.md), [`specs/00-conflicts.md`](../specs/00-conflicts.md) (C1)
**Source:** `docs/vesper-node-architecture.md` §1, §2, §7 F1
**Est. effort:** not stated in source

## Deliverables
- [ ] Fork the SDK, or keep a patch set tracked in this repo (decide and record which)
- [ ] Delete Meta transport: `vm_api.c`, `link_pairing.c`, `noise_control*`, `ble_server.c`, `muse_account_api.c`, `muse_chat_session.cpp`, `CONFIG_GADGET_SDK_TOKEN`
- [ ] New backend implementing the `muse_hatch_*` API (`turn_begin/audio/end/cancel/event/read/caption` + status), selected in `components/muse/CMakeLists.txt` (backend selection at :45-51)
- [ ] HTTPS note upload (WAV-header + 16 kHz PCM16, the shape the firmware already produces; reuse WAV/base64 helpers in `muse_chat_text.c`)
- [ ] SSE/chunked reply parsing: text deltas → captions; per-message TTS MP3 URL handed to the TTS slot (task 10)
- [ ] Repurpose `muse_settings.c` `host` (NVS `muse`) as our server URL
- [ ] Amend the protocol doc from task 07 if the firmware side needs changes

## Definition of done
- [ ] `git diff` shows `main/voice.c` unchanged
- [ ] `board.sh build aipi` builds clean with no Meta transport sources or `CONFIG_GADGET_SDK_TOKEN`
- [ ] On device: PTT turn → note reaches the node backend → reply captions appear
- [ ] Simulator `ctest` still green (or the simulator's backend selection is documented if it diverges)

## Anti-deliverables (do NOT build in this task)
- Changing `main/voice.c`, `board_aipi.c`, `voice_board.c`, `voice_player.c`, the LVGL UI, `muse_wifi`, `identity.c`, `ota.c`
- Proxying/MITM of Meta's transport (option (b), rejected)
- Reusing any Meta auth/token step
- Claim flow (task 11). Use a provisioned dev credential here

## Risks / unknowns
- HUMAN GATE: on-device verification needs Kevin to plug in the board
- Before task 12 the backend is reachable only on the LAN/6PN, so the device test needs a reachable URL

## Notes for Claude Code
- Start by reading the linked `specs/` files and, if needed, the original source doc.
- Tick off boxes in this file as you complete them.
- If a deliverable is blocked, append `> blocked: <reason>` under it rather than removing it.
- Commit incrementally; one logical change per commit.
