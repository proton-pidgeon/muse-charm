# HANDOVER.md — muse-charm

## 2026-10-03 — repo created, simulator task dispatched
- Repo `proton-pidgeon/muse-charm` (public) created for the DIY Muse Charm
  project. Starting point: get Meta's UI simulator running before buying
  hardware.
- `docs/simulator-setup.md` written from upstream
  `esp32/simulator/README.md` (muse-gadget-sdk @ 2026-10-03).
- `tasks/01-build-and-verify-simulator.md` dispatched to headless Claude on
  mac-daddy31337 via `/implement`.
- Next: review screenshots, run the interactive smoke test locally, then
  pick hardware (BOX-3B vs Waveshare 1.75C vs AiPi Lite — see research).

## 2026-10-03 — task 01 done: simulator builds, tests green, 5 renders
- **SDK commit built:** `facebookincubator/muse-gadget-sdk@b1a3822`
  ("Add M5Stack Cardputer ADV support (#10)"), shallow clone at
  `~/builds/muse-charm/scratch/muse-gadget-sdk` (outside this repo,
  unmodified). Build dir `esp32/simulator/build` there.
- Toolchain: cmake 4.4.3 + ninja 1.13.2 (brew-installed this session),
  AppleClang 17, Python 3.14.6. Clean Debug build, 541 steps, 0 warnings.
  CMake chose the downloaded/bundled `SDL2::SDL2` + `lvgl::lvgl`.
- `ctest`: 1/1 passed (`muse_simulator_headless`, ~1 s).
- **Screenshots** (412x412 P6 PPM, ~509 KB each, all distinct; PNG copies
  alongside for quick viewing): `~/builds/muse-charm/renders/`
  `{idle,listening,thinking,speaking,happy}.ppm` (+ `.png`).
  idle/listening/thinking used upstream scenarios; speaking + happy used
  scenario files written to `~/builds/muse-charm/renders/scenarios/`.
- **Doc corrections** (applied to `docs/simulator-setup.md`):
  1. Upstream ships only `error/idle/listening/pairing/thinking` scenarios;
     there is no `speaking.txt` or `happy.txt`; write your own (`face=` accepts
     `speaking` and `happy`).
  2. On this Mac the first configure failed: "C compiler is not able to compile a
     simple test program", `ld: tapi error ... unknown architecture
     arm64e.x1-macos`. Cause: `xcode-select` → Xcode 26.2 (ld-1230) but
     clang's default SDK is the newer CLT `MacOSX27.0.sdk`. Workaround used:
     `export SDKROOT=$(xcrun --sdk macosx --show-sdk-path)` (Xcode's 26.2 SDK)
     before configuring. Real fix: update Xcode to 27 or
     `sudo xcode-select -s /Library/Developer/CommandLineTools`.
  3. `--help` matches the doc; scenario keys also include `battery_mv`,
     `usb`, `charging`, `asleep`, `passkey`, `paired`, `speaker`,
     `brightness`, `level`. Unset keys get defaults (e.g. battery shows 72%).
- Next: human runs the interactive smoke test, then pick hardware.

## 2026-10-03 — task 02 scoped: live transport hack
- Code-reading verdict: typed-chat hack is feasible (clean `muse_hatch_*`
  seam; portable in-repo Noise+mbedTLS; `muse_state_set_caption` for
  replies). Voice is a separate, bigger task.
- Auth wall, tested: SDK token (`mgst_...`) → `fetch_vms` → 401. A device
  token (from app+BLE pairing of a real board) is required. Temp token
  material destroyed after the test.
- `tasks/02-sim-live-transport.md` written; token is runtime config only
  (env var or ~/.muse-charm/token), never in the repo.

## 2026-10-03 — task 02 cancelled by Kev
- The `/implement` run for task 02 was dispatched on a misread instruction
  and killed per Kev's explicit "kill the transport build" (local session +
  remote `claude -p` process both terminated; no partial commits).
- Task 02 file remains in `tasks/` as a scoped spec if revived later.
  Blocker stands: needs a device token (app+BLE pairing); SDK token → 401.

## 2026-10-03 — task 03 done: AiPi Lite bring-up prep (no hardware yet)
- **Runbook:** `docs/aipi-lite-bringup.md` (flash-day checklist, exact commands,
  port discovery, troubleshooting, clean re-flash, 3 human gates). Serial-log and
  pairing/verify sections are derived from SDK source and labelled EXPECTED, not observed.
- **Toolchain validated:** ESP-IDF v6.0.1 installed at `~/esp/esp-idf-v6.0.1`
  (Python env `~/.espressif/python_env/idf6.0_py3.14_env`, xtensa-esp-elf GCC 15.2.0,
  esptool 5.4.0). `esp32/tools/muse/board.sh build aipi` in the scratch SDK
  (`b1a3822`) succeeded with no board and no token: exit 0, 71 s wall, app
  0x211000 bytes (48% of the 4 MB slot free). Artifacts stay in the SDK dir; none in this repo.
  Simulator `ctest` still 1/1 green.
- **Token flow (corrects the shorthand above):** the `mgst_` SDK token (48 chars,
  from gadgets.muse.ai > Account > SDK tokens) is a BUILD-TIME Kconfig,
  `CONFIG_GADGET_SDK_TOKEN`, compiled into the firmware. Device hands it to the
  app inside the encrypted pairing session. The device token is separate: minted
  by app + BLE pairing, stored in NVS. A token-less build compiles with a CMake
  warning but will not pair, so flash day needs a token-set rebuild. The boot log
  prints the first 12 characters of the token.
- **Still hardware/human gated (Oct 8):** (1) Kevin fetches the SDK token,
  (2) Kevin plugs in the board over a data-capable USB-C cable (not included in
  the box), (3) Kevin pairs in Muse app > Settings > Devices > Developer mode.
  Unverified until then: how to enter download mode if esptool cannot connect
  (AIPI user buttons are GPIO42/GPIO1, not BOOT), actual serial output, voice round-trip.
