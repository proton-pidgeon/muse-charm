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
  warning but will not pair, so flash day needs a token-set rebuild. Serial output
  carries the first 12 characters of the token at three sites, not just boot: the
  boot banner (`main/app.c`), pairing confirmation (`main/link_pairing.c`) and every
  device-token refresh (`main/vm_api.c`). Treat any `mgst_` in a log as sensitive;
  redact with `sed -E 's/mgst_[A-Za-z0-9_-]+/mgst_REDACTED/g'` before sharing.
- **Every `idf.py` serial command needs `-B build-muse-aipi`** (monitor,
  erase-flash). The SDK's `sdkconfig.defaults` targets esp32c5, so a bare `idf.py`
  configures a stray `esp32/build/` + `esp32/sdkconfig` for the wrong chip; delete
  both if that happens. Runbook fixed accordingly.
- **Still hardware/human gated (Oct 8):** (1) Kevin fetches the SDK token,
  (2) Kevin plugs in the board over a data-capable USB-C cable (not included in
  the box), (3) Kevin pairs in Muse app > Settings > Devices > Developer mode.
  Unverified until then: how to enter download mode if esptool cannot connect
  (AIPI user buttons are GPIO42/GPIO1, not BOOT), actual serial output, voice round-trip.

## 2026-10-07 ~19:55 CDT — Vesper-node build kicked off (Kevin: 'take it all the way through to completion')
- /ingest of docs/vesper-node-architecture.md produced specs/ + tasks 04–13 (commit 9b87be9). Board #11 populated: issues #2–#11 (one per task), Status=Todo, Summary set. Issue #1 = epic.
- Kevin's decisions fed to tasks: v1 allowlist = chat + home/room control IN, purchases/messaging/irreversible OUT; transcripts transient-only (no logging); PTT for v1; TTS = phone brain's ElevenLabs voice (VESPER_PHONE_TTS_VOICE_ID).
- Wave 1 dispatched: task 04 (A1 avatar) on Studio; task 06 (B3 channel=node) on ravenz-node.
- B4 (task 12, Peggy deploy) HELD for Kevin's explicit approval — build everything else first.

## 2026-10-07 ~20:13 CDT — Task 04 GREEN (coordinator lost to runtime drain, work verified)
- /implement merged impl/04: evening-star owl avatar (7f1ea58) — firmware/avatar/ with 8 mode GIFs, bench.c, install.sh. Board issue #2 → Done.
- Task 05 (A2 on-device validation) dispatched to Studio.
- Task 06 (B3) still running on ravenz-node.

## 2026-10-07 ~20:30 CDT — Task 05 (A2): Vesper avatar FLASHED, awaiting Kevin's eyes on the LCD
- Board was already plugged in (/dev/cu.usbmodem83201) and paired. Flashed the avatar-only build (SDK b1a3822 untouched + firmware/avatar/muse_pixel.c; ELF cf6b49a47…, built 20:08) over the 18:45 stock build. App-only flash, NVS kept: boots paired, VM connected, no panic, heap same as stock.
- Spec correction: the AIPI canvas is 96 px (stock muse_ui.c `s_h*3/4`), so the owl renders at 1.5×, not an exact 2× to 128×128.
- ~20:19: all 8 modes verified from the device's own framebuffer. A MUSE_BENCH=1 build (snapshot only) plus `snap.py '>face=<mode>'` → firmware/avatar/device-snaps/ (sheet.png). Voice path OK on that build: serial-PTT + macOS `say` → 4.7 s note → 81-char reply + 7 s audio, turn 15.1 s.
- ~20:25: reflashed the canonical non-bench build (cf6b49a4). **Board then came up UNPAIRED + no Wi-Fi creds** (`boot: unpaired - advertising`). It booted twice in a row, which fits a setup reset (wipes Wi-Fi + app pairing, then restarts). The flash itself doesn't write NVS, and pairing survived the earlier bench flash. SDK triggers: Muse menu "Reset pairing", server `node.unpaired`, deferred reset. Which one fired wasn't captured. If Kevin touched the menu around 20:22–20:25, that's the answer; otherwise suspect a server-side unpair and watch for it on the next pairing.
- KEVIN TODO: (1) re-pair: Muse app > Settings > Devices > Add Device, then press the talk button (Wi-Fi gets provisioned over BLE again). (2) one normal talk-button turn, watching the screen: IDLE gold star → hold = LISTENING cyan → THINKING magenta → SPEAKING mint + caption → happy hop. ERROR/OFF are already framebuffer-verified (OFF = hold the bottom-left button 1.5 s). Then task 05 can close.
- Rollback to stock avatar: `rm ~/builds/muse-charm/scratch/muse-gadget-sdk/esp32/components/muse/avatar/muse_pixel.c`, then `tools/muse/board.sh build aipi && tools/muse/board.sh flash aipi`.

## 2026-10-07 ~20:18 CDT — Task 06 GREEN (B3: channel=node)
- /implement on ravenz-node merged to vesper-voice main @ 212a79f: node variant in persona.py::system_prompt, channel-aware to_spoken in ask.py, /ask accepts channel (allowlisted ask/node, default ask, invalid→422, no transcript logging). +9 tests; 443 passed, 21 pre-existing Windows-env failures (zero growth, byte-identical baseline); ruff clean. Live brain untouched.
- Board issue #4 → Done, closed.
- Task 07 (B1 backend) next: Studio after 05 (task is Studio-homed: launchd + keys + latency measurement). Decisions to inject: TTS = phone brain's ElevenLabs voice (Q1 resolved); transcripts transient-only (Q4 default); HTTP shim, no LiveKit spike (Q6); PTT v1.

## 2026-10-07 ~20:30 CDT — Task 05 verified on-device, one human gate to close
- Avatar-only build flashed; all 7 modes + happy overlay verified via LVGL framebuffer screenshots (firmware/avatar/device-snaps/, 11 shots). Voice path verified (PTT turn over serial). Commits ce12fd8, 9e63b0c.
- Spec correction: UI draws the owl at 96px (1.5x) on the 128px panel, not exact 2x — UI untouched, as designed.
- HUMAN GATE: board came up unpaired/Wi-Fi-wiped after the final flash (cause undetermined; pairing survived the earlier bench flash). Needs Kevin: re-pair in Muse app + one watched PTT turn to fully close task 05. Board issue #3 stays In Progress.
- Task 07 (B1 backend) dispatched to Studio. Vesper firmware (09+) replaces Meta pairing with the claim flow, so the re-pair is task-05-closure only, not a build blocker.
