# firmware/

Vesper-specific firmware pieces that sit on top of the unmodified Meta Muse gadget SDK
(clone at `~/builds/muse-charm/scratch/muse-gadget-sdk`, commit `b1a3822`).

## Decision: the avatar is a patch set tracked in THIS repo (not an SDK fork)

`firmware/avatar/muse_pixel.c` is the source of truth. `firmware/avatar/install.sh [SDK_DIR]`
copies it (idempotently) to `<SDK>/esp32/components/muse/avatar/muse_pixel.c`.

Why:

- The SDK already has an explicit hook for this: `components/muse/CMakeLists.txt` globs
  `components/muse/avatar/muse_pixel.c` and uses it in place of the default avatar. That directory
  is ignored by the SDK repo, so installing the file touches no tracked SDK file and the SDK stays
  a pristine, re-clonable upstream checkout.
- The avatar is independent of transport. Task 09 (F1) still decides fork-vs-patch for the
  transport; this choice does not constrain it. If F1 forks the SDK, the install step becomes a
  plain copy into the fork (or the file moves there) with no code change.
- The art is Vesper-owned; keeping it here keeps it out of any Meta-derived tree and reviewable
  next to the specs.

Caveat: the simulator (`esp32/simulator/CMakeLists.txt`) hard-codes the default avatar path
(`../../avatar/muse_pixel.c`) and does not use the custom one. The simulator `ctest` therefore does
not exercise Vesper's renderer; the avatar is covered by `make_gifs.py` (host render of every
mode) and by the ASan/UBSan bench run below. Making the simulator use it would mean editing a
tracked SDK file, which this task does not do.

## The avatar: Vesper, the evening-star owl

Original 64x64 palette-indexed art (31 palette entries, Bayer-dithered shading, 1 px outline).
A plump indigo owl with a pale moon face disc, ear tufts, big dark glinting eyes, small gold beak
and feet, and the evening star (a four-point sparkle) on its forehead. The star, aura, sparkles
and UI accent take the mode colour: gold at idle (a deliberate deviation from the SDK's violet
default), cyan listening, magenta thinking, mint speaking, red error, white/blue boot, dusk
violet off.

| Mode | Behaviour |
|---|---|
| BOOT | pops up from a squash, eyes open at 0.9 s, star ignites at 1.1 s, sparkles appear one by one |
| IDLE | slow bob, wings sway, random ear flicks, blinks (with double blinks), wandering gaze, pulsing star |
| LISTENING | ears perked, wide eyes, small open beak, raised brows, wings raised like cupped ears, expanding rings and sound arcs, star pulses with `level` |
| THINKING | eyes glance up and side to side, "hmm" beak, one wing folded to the chin, lean, one ear up, stepping thought dots, fast star flicker |
| SPEAKING | beak opens with `level` (plus flutter), body bobs with the voice, wings gesture, feet shuffle, rings and arcs, star flares with `level` |
| ERROR | X eyes, flat beak, 0.6 s shake, drooping ears, "!" mark, red scheme, star flickers (`happy` ignored) |
| OFF | right wing waves goodbye, eyes close, ears droop, glow fades over 1.3 s |
| happy overlay | hop, wings up and wiggling, `^^` eyes, open grin, ears up, big star, two hearts rising |

Previews: `firmware/avatar/gifs/*.gif` (192 px, one per mode plus `happy`), generated with
`python3 tools/muse/make_gifs.py` in the SDK and shrunk by `firmware/avatar/shrink_gifs.py`.

## Render time vs the 10 ms S3 budget

`firmware/avatar/bench.c` times `muse_pixel_render()` plus a full 128x128 `muse_pixel_scale()` per
frame, for every mode with and without the happy overlay (1950 frames each, `cc -O2`):

    cc -O2 -I <SDK>/esp32/components/muse firmware/avatar/bench.c firmware/avatar/muse_pixel.c -lm -o /tmp/vesper_bench && /tmp/vesper_bench

Host (Apple M4 Max, arm64, -O2): render mean 11-15 us (max seen 53 us), scale128 mean ~3 us;
worst single frame render+scale 48 us. The same code builds clean with
`-Wall -Wextra -Werror` and runs clean under AddressSanitizer + UBSan.

S3 estimate (240 MHz Xtensa LX7, 1 single-precision FPU, no sqrt/sin hardware): the M4 is roughly
150-250x faster per core on this kind of scalar integer code (about 4.5 GHz at ~6 IPC vs 240 MHz at
~0.7-1 IPC). That puts render at about 2-4 ms and a full scale at about 0.5-1 ms: roughly
3-5 ms per frame, comfortably under 10 ms even with a 2x safety margin for internal-RAM cache
misses. Per-pixel work is Q12 integer inside the body, wing, face and aura bounding boxes; floats
are used per frame or per part only: about 70 sqrtf (one per body row), a few dozen
sinf/cosf/expf, and the tiny eye/blush/foot ellipses (a few tens of pixels). On the S3's FPU these
are a few thousand cycles in total (under 0.3 ms), so there is no float/libm hot spot. No
allocation; static RAM is about 8 KiB of planes plus palettes and a 512-byte map. This is a host
proxy, not a measurement; confirm on hardware in task 05 if desired.
