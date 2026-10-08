# Task 05: Avatar on-device validation (A2)

**Goal:** An avatar-only firmware build (stock transport untouched) is flashed to the AiPi Lite and every mode is confirmed on the 128×128 LCD.
**Depends on:** 04
**Relevant specs:** [`specs/01-vesper-node-architecture.md`](../specs/01-vesper-node-architecture.md)
**Source:** `docs/vesper-node-architecture.md` §5, §7 A2
**Est. effort:** not stated in source

## Deliverables
- [x] Avatar-only firmware build: task 04's `muse_pixel.c` + unmodified stock transport
  > 2026-10-07: SDK `b1a3822` clean (`git status` empty; avatar lives in the ignored `components/muse/avatar/`, byte-identical to `firmware/avatar/muse_pixel.c`). `build-muse-aipi` compiled 2026-10-07 20:08:46, token set (48 chars), ELF SHA256 `cf6b49a47…`.
- [x] Flash procedure per `docs/aipi-lite-bringup.md`
  > 2026-10-07: `tools/muse/board.sh flash aipi` on `/dev/cu.usbmodem83201` (303A:1001) → `Hash of data verified`. Replaced the 18:45 stock build (ELF `db652f09f…`). NVS kept: boots paired, wifi up, VM connected in ~2.2 s, no panic; heap matches stock (int 32K free vs 33K).
- [x] Check each of the 7 modes + happy overlay on the device (exact 2× upscale to 128×128 RGB565)
  > 2026-10-07: verified from the device's own LVGL framebuffer: a `MUSE_BENCH=1` build (adds only `CONFIG_LV_USE_SNAPSHOT=y`, separate `build-muse-aipi-bench/`) with `tools/muse/snap.py PORT '>face=<mode>'` for every mode. All 8 render correctly: see `firmware/avatar/device-snaps/` (`sheet.png` = boot·idle·listening·thinking / speaking·error·off·happy; `boot_early/late`, `off_early/late` show the boot ignite and the off fade). After that, the canonical non-bench build was reflashed.
  > spec correction: on the AIPI the stock `muse_ui.c:1499` sets the canvas to `s_h*3/4` = **96 px** (boot log: `UI up: 128x128, 96 px Muse`), so the 64×64 art is upscaled **1.5×**, not an exact 2× to 128×128. The LVGL UI stays untouched per spec, so this is expected behaviour; check the owl reads well at 96 px.
- [ ] HANDOVER.md entry with observations (photos/notes if Kevin supplies them)
  > partial: entries written 2026-10-07; Kevin's own look at the physical LCD still pending.

## Definition of done
- [ ] All 7 modes + happy overlay observed on the physical LCD (BOOT/IDLE/LISTENING/THINKING/SPEAKING reachable in a normal PTT turn; ERROR/OFF by inducing them where practical)
  > framebuffer-verified on the device (above); only a quick look at the physical panel remains (Kevin).
- [ ] Stock voice path still behaves as on flash day (button → listening → caption reply)
  > 2026-10-07: on the bench build, a PTT turn driven by serial `d`/`u` (posts the same PTT event the talk button does), with macOS `say` speaking the question, completed: recorded 4.7 s → `chat/stream` ack +849 ms → 81-char reply +5.0 s → 7.06 s of reply audio, turn done 15.1 s. Still pending: the physical button + caption seen by eye.
  > blocked: the board is now UNPAIRED (see HANDOVER 2026-10-07 ~20:25). After the final canonical reflash it rebooted twice and came up `boot: unpaired`, `no wifi creds`. That is a setup reset, which wipes Wi-Fi + pairing. A flash doesn't do it (`flash_args` doesn't write NVS @0x11000, and pairing survived the bench flash). Triggers in the SDK: the Muse menu "Reset pairing", the server-side `node.unpaired`, or a deferred reset. Which one fired wasn't captured. Needs Kevin to re-pair (runbook human gate 3).

## Anti-deliverables (do NOT build in this task)
- Transport changes; stock Meta transport stays untouched in this build

## Risks / unknowns
- HUMAN GATE: needs Kevin to plug the board in via USB-C and watch the screen

## Notes for Claude Code
- Start by reading the linked `specs/` files and, if needed, the original source doc.
- Tick off boxes in this file as you complete them.
- If a deliverable is blocked, append `> blocked: <reason>` under it rather than removing it.
- Commit incrementally; one logical change per commit.
