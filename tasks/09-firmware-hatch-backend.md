# Task 09: SDK fork + third `muse_hatch_*` backend (F1)

**Goal:** The AiPi firmware runs a third `muse_hatch_*` backend that speaks our protocol to the node backend, with Meta's transport removed and `main/voice.c` unchanged.
**Depends on:** 07 (B1 live), protocol doc from 07
**Relevant specs:** [`specs/01-vesper-node-architecture.md`](../specs/01-vesper-node-architecture.md), [`specs/00-conflicts.md`](../specs/00-conflicts.md) (C1)
**Source:** `docs/vesper-node-architecture.md` §1, §2, §7 F1
**Est. effort:** not stated in source

## Deliverables
- [x] Fork the SDK, or keep a patch set tracked in this repo (decide and record which)
  - Decision: a patch set, recorded in `firmware/README.md`. It is `firmware/apply-sdk.sh` + `firmware/sdk-patches/` (`delete.txt` and 3 patches) + `firmware/hatch/`.
- [x] Delete Meta transport: `vm_api.c`, `link_pairing.c`, `noise_control*`, `ble_server.c`, `muse_account_api.c`, `muse_chat_session.cpp`, `CONFIG_GADGET_SDK_TOKEN`
  - Also deleted: the code only those files used (`noise_tunnel*`, `tunnel_netif`, `net_discovery`, `pairing_*`, `factory_test`, `bug_report`, `image_fetch`, `muse_chat_link.c`, `components/noise_core`).
- [x] New backend implementing the `muse_hatch_*` API (`turn_begin/audio/end/cancel/event/read/caption` + status), selected in `components/muse/CMakeLists.txt` (backend selection at :45-51)
  - The backend is `firmware/hatch/muse_chat_vesper.c`.
- [x] HTTPS note upload (WAV-header + 16 kHz PCM16, the shape the firmware already produces; reuse WAV/base64 helpers in `muse_chat_text.c`)
  - It is a raw chunked POST, and the WAV header comes from `muse_hatch_wav_header`. There is no base64 (protocol v1). Plain `http://` is also accepted for LAN, 6PN and `[::1]` URLs.
- [x] SSE/chunked reply parsing: text deltas → captions; per-message TTS MP3 URL handed to the TTS slot (task 10)
  - The slot is `vesper_tts_slot_offer()`. It is a weak default that declines, so replies stay captions over silence until task 10.
- [x] Repurpose `muse_settings.c` `host` (NVS `muse`) as our server URL
  - `host` now holds the server base URL. The credential is a new key, `muse:node_token`. The Kconfig defaults are empty.
- [x] Amend the protocol doc from task 07 if the firmware side needs changes
  - Added a *Node firmware notes* section. The wire shape is unchanged, so it stays v1.

## Definition of done
- [x] `git diff` shows `main/voice.c` unchanged
- [x] `board.sh build aipi` builds clean with no Meta transport sources or `CONFIG_GADGET_SDK_TOKEN`
  - Verified from a fresh `b1a3822` worktree after `firmware/apply-sdk.sh`. The name survives in exactly one place, a `CONFIG_GADGET_SDK_TOKEN=""` compile definition for the frozen `identity.c`, explained in `firmware/README.md`.
- [ ] On device: PTT turn → note reaches the node backend → reply captions appear
> blocked: HUMAN GATE. The agent must not flash the board, which still runs Meta firmware for task 05. The host side is already proven: `make -C firmware live-turn` ran a real turn against `com.vesper.node` through the firmware's own protocol code. Steps for Kevin (also in `firmware/README.md`):
> 1. Make the backend reachable from the board. Before task 12 the backend binds `::`, which is IPv6-only on the Studio, so run e.g. `socat TCP4-LISTEN:8797,fork,reuseaddr TCP6:[::1]:8796` and use `http://<studio-LAN-IPv4>:8797`. After task 12, use `https://peggy.fly.dev/vesper-node`.
> 2. Create a fresh SDK worktree: `git -C ~/builds/muse-charm/scratch/muse-gadget-sdk worktree add --detach ~/builds/muse-charm/scratch/sdk-vesper b1a3822`.
> 3. Apply the patch set: `firmware/apply-sdk.sh ~/builds/muse-charm/scratch/sdk-vesper`.
> 4. Build and flash: `cd ~/builds/muse-charm/scratch/sdk-vesper/esp32 && tools/muse/board.sh build aipi && tools/muse/board.sh flash aipi /dev/cu.usbmodem83201`.
> 5. Open the monitor with `. ~/esp/esp-idf-v6.0.1/export.sh && idf.py -B build-muse-aipi -p /dev/cu.usbmodem83201 monitor`. Type `>wifi.ssid=<net>`, `>wifi.pass=<pw>`, `>wifi.connect`, `>hatch.host=<url>`, `>hatch.token=<VESPER_NODE_TOKEN from ~/.config/vesper-voice/node.env>` and `>hatch.test`. You should see `server check: HTTP 200`.
> 6. Hold the talk button, say "what is two plus two", and release. You should see the transcript and then the reply caption on screen. The serial log should show `turn: HTTP 200` … `turn done`.
- [x] Simulator `ctest` still green (or the simulator's backend selection is documented if it diverges)
  - `ctest` passes 1/1 on the patched tree. The simulator compiles no transport, as documented in `firmware/README.md`.

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
