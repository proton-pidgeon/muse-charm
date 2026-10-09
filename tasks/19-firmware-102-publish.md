# Task 19: Build + publish firmware 1.0.2 (timer/reminder announcements)

## Goal
Build firmware 1.0.2 from main (f95e635, task 18 announcements merged) and publish it OTA via `vesper-node firmware publish` so the board can speak timer/reminder announcements.

## Context
- Task 18 merged `f95e635`: `GET /announcements` backend + firmware 1.0.2 idle 15s announcement poll. A 1.0.2 binary was built during task 18 (2,035,712 B) but NOT published. 1.0.1 is currently published.
- Known gotcha: `board.sh` in the current SDK tree lacks the `aipi` target — use the idf.py path from `docs/aipi-lite-bringup.md`.

## Steps
1. In `/Users/k3v/builds/muse-charm/scratch/sdk-impl-18-fresh/esp32` (the tree with the 1.0.2 code):
   - `. ~/esp/esp-idf-v6.0.1/export.sh`
   - `idf.py -B build-muse-aipi -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-muse-aipi/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-aipi" build`
2. Verify: `build-muse-aipi/muse-gadget.bin` is 2,035,712 bytes and contains version string `1.0.2`.
3. Publish: `vesper-node firmware publish <path-to-bin>` from the muse-charm backend venv.
4. Verify: `vesper-node firmware status` shows 1.0.2.
5. Confirm the board (homelink-c86320) picks it up: check the backend log for the OTA manifest check / download, or have Kevin confirm the version on-device.

## Done when
- `firmware status` reports 1.0.2 published.
- Board confirmed on 1.0.2 (log evidence or Kevin's confirmation).
- HANDOVER.md updated; board issue closed with dotted Summary.

## Constraints
- Firmware work on the Studio only. No secrets in logs. Green build required before publish.
