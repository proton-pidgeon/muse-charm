# Task 05: Avatar on-device validation (A2)

**Goal:** An avatar-only firmware build (stock transport untouched) is flashed to the AiPi Lite and every mode is confirmed on the 128×128 LCD.
**Depends on:** 04
**Relevant specs:** [`specs/01-vesper-node-architecture.md`](../specs/01-vesper-node-architecture.md)
**Source:** `docs/vesper-node-architecture.md` §5, §7 A2
**Est. effort:** not stated in source

## Deliverables
- [ ] Avatar-only firmware build: task 04's `muse_pixel.c` + unmodified stock transport
- [ ] Flash procedure per `docs/aipi-lite-bringup.md`
- [ ] Check each of the 7 modes + happy overlay on the device (exact 2× upscale to 128×128 RGB565)
- [ ] HANDOVER.md entry with observations (photos/notes if Kevin supplies them)

## Definition of done
- [ ] All 7 modes + happy overlay observed on the physical LCD (BOOT/IDLE/LISTENING/THINKING/SPEAKING reachable in a normal PTT turn; ERROR/OFF by inducing them where practical)
- [ ] Stock voice path still behaves as on flash day (button → listening → caption reply)

## Anti-deliverables (do NOT build in this task)
- Transport changes; stock Meta transport stays untouched in this build

## Risks / unknowns
- HUMAN GATE: needs Kevin to plug the board in via USB-C and watch the screen

## Notes for Claude Code
- Start by reading the linked `specs/` files and, if needed, the original source doc.
- Tick off boxes in this file as you complete them.
- If a deliverable is blocked, append `> blocked: <reason>` under it rather than removing it.
- Commit incrementally; one logical change per commit.
