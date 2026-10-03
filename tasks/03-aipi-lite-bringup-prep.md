# Task 03 — AiPi Lite bring-up prep (no hardware yet)

## Context
Kevin ordered the red AIPI Lite (X-Origin, ESP32-S3, 128×128 LCD, mic + speaker,
magnetic case + battery). It arrives **Thursday, October 8**. This task prepares
EVERYTHING needed so that flash day is mechanical: read docs, validate the
toolchain, write the runbook. No hardware is available until delivery — do not
wait for it; everything here is doable without the board.

Meta's SDK (`facebookincubator/muse-gadget-sdk`, Apache 2.0) has first-class
AiPi support:
- Board driver: `esp32/components/muse/boards/board_aipi.c`
- Build profile: `esp32/devices/sdkconfig.muse-aipi`
- Build/flash: `esp32/tools/muse/board.sh build|flash aipi [port]`
  (needs ESP-IDF v6.0.1 with `idf.py` on PATH; build log goes to
  `/tmp/muse_build_aipi.log`; flash finds the port via `tools/muse/ports.py`)
- gadgets.muse.ai lists AiPi Lite as an official Muse Gadget example with
  source — flashing Muse firmware is a supported path, not a hack.

Prior art in this repo: task 01 built the desktop simulator
(`muse_simulator`, ctest green). Task 02 (live transport) was cancelled.
Do not touch the simulator's behavior.

## Deliverables
1. **`docs/aipi-lite-bringup.md`** — the complete flash-day runbook:
   - What's in the box (device, 450 mAh battery module, quick-start guide;
     **USB-C cable and power adapter are NOT included** — bring a DATA-capable
     USB-C cable, not a charge-only one).
   - Prerequisites with exact versions (ESP-IDF v6.0.1, esptool via idf.py,
     macOS USB serial driver if needed).
   - SDK token: read the SDK source/docs and pin down the EXACT token flow —
     where Kevin gets the token (gadgets.muse.ai), where it goes
     (build-time config? BLE provisioning? app pairing?), and its format.
     Mark this as a HUMAN GATE: Kevin fetches it himself; never commit, print,
     or log token material.
   - Build: exact commands, expected output, how long it takes.
   - Flash: exact commands, how to find the port, what success looks like
     (serial log snippets).
   - Pair: Muse app → Settings → Devices → Developer mode (HUMAN GATE —
     needs Kevin's phone).
   - Verify: expected boot behavior, avatar on the 128×128 LCD, button
     push-to-talk, first voice round-trip.
   - Troubleshooting: port not found, flash failures, boot loops, Wi-Fi
     provisioning issues, how to do a clean re-flash.
   - A short "flash-day checklist" Kevin can follow top to bottom.
2. **Toolchain validated on this machine**: set up ESP-IDF v6.0.1 if missing,
   run `tools/muse/board.sh build aipi` to completion. Record the exact
   environment (versions, paths) in the runbook. Do NOT commit build
   artifacts or binaries — document how to reproduce instead.
3. **HANDOVER.md entry** summarizing what was validated and what remains
   hardware-gated.

## Constraints
- No hardware until Oct 8: build must succeed without a board attached;
  flash/pair/verify steps are documented, not executed.
- Token material (if encountered in SDK docs/examples) never enters git,
  logs, or chat.
- Keep the existing simulator build green (`ctest` in
  `esp32/simulator/build`).
- Work on a branch `task/03-aipi-bringup`, open a PR only if the change is
  high-blast-radius (it shouldn't be — docs + env setup). Otherwise commit
  to the branch and report; Vesper merges.

## Human gates (do NOT attempt; document them)
1. Kevin fetches the SDK token from gadgets.muse.ai.
2. Kevin plugs the board in via USB-C on flash day.
3. Kevin pairs in the Muse app (Settings → Devices → Developer mode).

## Done when
- `docs/aipi-lite-bringup.md` exists and is complete per above.
- `board.sh build aipi` completes cleanly on this host; env recorded.
- Simulator `ctest` still green.
- HANDOVER.md updated; branch pushed; report posted with the exact flash-day
  commands and the list of human gates.
