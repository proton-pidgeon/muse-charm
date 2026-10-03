# Task 01: Build and verify the Muse Gadget UI simulator

**Goal:** Get the Meta Muse Gadget UI simulator building and passing its
tests on this Mac, and produce headless screenshot renders of the avatar
states, following `docs/simulator-setup.md`.
**Depends on:** none
**Relevant docs:** [docs/simulator-setup.md](../docs/simulator-setup.md);
upstream `esp32/simulator/README.md` in
`facebookincubator/muse-gadget-sdk` (wins on conflict).
**Est. effort:** small — no ESP-IDF, no hardware.

## Deliverables
- [ ] Prereqs present: Xcode CLT, `cmake` ≥ 3.24, `ninja`, Python ≥ 3.9
  (`brew install cmake ninja python` if missing)
- [ ] `muse-gadget-sdk` cloned (shallow is fine) into a scratch dir OUTSIDE
  this repo — do not vendor the SDK here
- [ ] `cmake -S esp32/simulator -B esp32/simulator/build -G Ninja -DCMAKE_BUILD_TYPE=Debug`
  then `cmake --build esp32/simulator/build --parallel` — clean build
- [ ] `ctest --test-dir esp32/simulator/build --output-on-failure` — green
- [ ] Headless renders (this SSH session has no display — use `--headless`):
  `--scenario` + `--run-ms 250` + `--screenshot` for `idle`, `listening`,
  `thinking`, `speaking`, and `happy`; save the PPMs somewhere durable and
  note their paths in HANDOVER.md
- [ ] `muse_simulator --help` runs; record anything surprising vs the doc

## Definition of done
- [ ] `ctest` passes with no failures
- [ ] Five headless screenshots render (non-zero-size PPMs, one per state)
- [ ] HANDOVER.md updated with: SDK commit built, screenshot paths, any
  doc corrections

## Anti-deliverables (do NOT do in this task)
- Modify the SDK source — build it as-is
- Commit the SDK or its build dir into this repo
- Interactive GUI testing (no display over SSH) — headless only; the
  human runs the interactive smoke test from `docs/simulator-setup.md`
- Buy hardware; flash anything

## Risks / unknowns
- First CMake configure downloads SDL 2.32.10 + LVGL 9.5.0 — needs
  network; if the fetch fails, retry before diagnosing further
- If `brew` is missing or Xcode CLT install prompts interactively, stop
  and report rather than working around it
