# Task 14: Iconic avatar redesign (A3)

**Goal:** Replace the evening-star owl avatar with a procedurally drawn "Iconic" cyborg face — the direction Kevin chose (2026-10-08) from three pixel-art sketches. Bold, graphic, severe cyberpunk. Absolutely NOT cute.
**Depends on:** 04, 05 (avatar pipeline + on-device validation pattern), 13 (dispatch after 13 finishes — one task per host)
**Relevant specs:** [`specs/01-vesper-node-architecture.md`](../specs/01-vesper-node-architecture.md), `firmware/avatar/muse_pixel.c` (predecessor — retire the owl)
**Reference sketch:** `/Users/k3v/builds/muse-charm/avatar-ref-iconic.webp` (on this host — VIEW IT FIRST, internalize the direction before writing code)
**Source:** Kevin's design call 2026-10-08 ~12:45 CDT ("Let's try 2" — the Iconic direction)

## The design (from the reference sketch)
- Front-facing cyborg woman, reduced to strong readable shapes at 64×64
- THE focal point: a huge glowing cyan cybernetic eye — concentric square rings with circuit traces — dominating one side of the face
- Silver-gray hair as angular swept planes (no soft curves, no baby-owl roundness)
- One natural eye (blue), severe neutral expression — calm and formidable, never adorable
- Circuit traces running down the implant side of the face
- Dark collar hint at the bottom; solid black background
- Palette discipline: ~32 colors like the owl, procedural (no bitmaps)

## Deliverables
- [x] Successor to `firmware/avatar/muse_pixel.c`: procedural Iconic face renderer, 64×64 palette-indexed, honoring the `muse_pixel.h` contract (same API boundary the owl used — keep the hardened boundary from task 04's review)
  > 2026-10-08: same file name (the SDK build glob and make_gifs pick it up), 32 palette entries, polygons + rules only, Q12 integer per-pixel work, static memory only. Boundary kept and extended (NULL pose, NaN/inf/huge times and levels, out-of-range modes, set_size/scale bounds); hostile-pose run clean under ASan + UBSan.
- [x] All 7 modes render: BOOT, IDLE, LISTENING, THINKING, SPEAKING, ERROR, OFF — KEEP the existing accent-color scheme (gold idle, cyan listening, magenta thinking, mint speaking, red error); the cybernetic eye glow should take the mode accent color
- [x] Mode behaviors: listening (eye brightens/scans), thinking (circuit pulse), speaking (subtle mouth/activity indicator), error (red) — severe, not playful
  > boot = scan-line draw-in + ring-by-ring ignite; off = eye collapses to its core and goes dark, natural eye closes; happy = acknowledge flare (no smile). Table in `firmware/README.md`.
- [x] GIF previews for every mode in `firmware/avatar/gifs/` (follow the task 04 pattern)
- [x] Framebuffer verification on the real AiPi Lite for all modes (task 05 pattern)
  > `MUSE_BENCH=1` build of `sdk-impl-13-fresh`, `snap.py '>face=<mode>'` for all 7 modes + happy; all render correctly at the 96 px canvas (1.5×).
- [x] Retire the owl: remove or clearly supersede the owl renderer; the Iconic face is the DEFAULT display avatar in all firmware builds going forward
  > owl source replaced (git history only); `apply-sdk.sh`/`install.sh` install the Iconic face; also installed in the pristine `scratch/muse-gadget-sdk` clone (install.sh's default).
- [x] Device snaps in `firmware/avatar/device-snaps/` showing the new avatar on the 128×128 display

## Definition of done
- [x] Green build + adversarial review per the /implement gates; merged to main
  > green gate done by the implementer (make -C firmware test, -Werror host build + bench, board.sh build aipi with the custom avatar). Review 2026-10-08: GPT-5 raised one HIGH (`muse_pixel_scale` trusts `stride_px`); Fable ruled it ADVISORY (pre-existing in the owl, unreachable from both muse_ui.c callers) and found nothing else; the guard was applied anyway (ede835e). Merged bc639c8; `make -C firmware test` green on main.
- [x] Final firmware flashed to the REAL AiPi Lite; the 128×128 display shows the Iconic face (device snap as proof)
  > canonical (non-bench) 1.0.0 build of `sdk-impl-13-fresh` with the Iconic avatar flashed 2026-10-08 and booted clean; snaps are from the bench build of the same tree and avatar source. Review didn't change the art; after merge, main was rebuilt (compile time 13:28:03, 2,035,712 B) and reflashed (`Hash of data verified`), boots clean, Wi-Fi up, no panic.
- [ ] HUMAN GATE: Kevin sees the rendered result on the device (or a faithful snap) and approves the look — he picked the direction, he confirms the execution
  > pending: show Kevin `firmware/avatar/device-snaps/sheet.png` (or the board itself).

## Anti-deliverables (do NOT build in this task)
- Photorealism — impossible in 64×64 procedural pixel art; stay graphic and iconic
- New modes, new accent colors, or changes to the mode state machine — keep the existing scheme
- Cuteness. If a reviewer could call it "cute," redraw it.

## Risks / unknowns
- HUMAN GATE: Kevin's aesthetic approval of the final render (he is fast with verdicts — show him the device snap)
- The 96px canvas finding from task 05 still applies — verify against it

## Notes for Claude Code
- Start by viewing the reference sketch and reading `firmware/avatar/muse_pixel.c` to learn the contract, palette discipline, and perf budget (Q12 fixed point, static memory, no allocation).
- Tick off boxes in this file as you complete them.
- If a deliverable is blocked, append `> blocked: <reason>` under it rather than removing it.
- Commit incrementally; one logical change per commit.
