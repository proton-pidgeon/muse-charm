# Task 04: Original Vesper pixel avatar (A1)

**Goal:** A Vesper-original custom `muse_pixel.c` covering all 7 SDK modes + happy overlay builds into the AiPi firmware and previews as GIFs without hardware.
**Depends on:** none (SDK clone from task 03 at `~/builds/muse-charm/scratch/muse-gadget-sdk`)
**Relevant specs:** [`specs/01-vesper-node-architecture.md`](../specs/01-vesper-node-architecture.md)
**Source:** `docs/vesper-node-architecture.md` §5, §7 A1
**Est. effort:** not stated in source

## Deliverables
- [x] Custom `components/muse/avatar/muse_pixel.c` implementing `muse_pixel.h` (`render(pose)` / `scale()` / `set_size()` / `accent(mode)`), picked up by the `components/muse/CMakeLists.txt` glob
- [x] 64×64 palette-indexed art, Vesper-original (Meta's default character is not rights-granted); reference only: `~/workspace/avatars/.frames/`
- [x] All 7 modes (BOOT / IDLE / LISTENING / THINKING / SPEAKING / ERROR / OFF) + happy overlay; SDK modes mapped to Vesper personality (IDLE/LISTENING/THINKING/SPEAKING are the live ones; BOOT/ERROR/OFF for completeness)
- [x] GIF previews for every mode via `tools/muse/make_gifs.py`
- [x] Decide and record where the avatar lives (SDK fork vs patch set tracked in this repo, per F1's "Fork the SDK (or patch set tracked in this repo)")

## Definition of done
- [x] `tools/muse/make_gifs.py` produces a GIF per mode + the happy overlay; reviewed by eye
- [x] `tools/muse/board.sh build aipi` completes with the custom avatar
- [x] Per-frame render time measured or estimated against the <10 ms S3 frame budget, with the result recorded
- [x] Simulator `ctest` (`esp32/simulator/build`) still green

## Anti-deliverables (do NOT build in this task)
- Converting the 1600×1600 photorealistic frames. They are "reference/inspiration, not convertible assets"
- Any transport/network change (the avatar "lands independently of transport work")
- Flashing the device (that's task 05)

## Risks / unknowns
- The <10 ms frame budget on the S3 can't be measured without hardware, so a host-side estimate is only a proxy

## Notes for Claude Code
- Start by reading the linked `specs/` files and, if needed, the original source doc.
- Tick off boxes in this file as you complete them.
- If a deliverable is blocked, append `> blocked: <reason>` under it rather than removing it.
- Commit incrementally; one logical change per commit.
