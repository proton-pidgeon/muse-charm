# Task 13: OTA + fleet hygiene (F4)

**Goal:** `ota.c` updates nodes from our server so a multi-room fleet can be updated without USB.
**Depends on:** 09, 12
**Relevant specs:** [`specs/01-vesper-node-architecture.md`](../specs/01-vesper-node-architecture.md)
**Source:** `docs/vesper-node-architecture.md` §6, §7 F4
**Est. effort:** not stated in source

## Deliverables
- [x] Keep `ota.c` working against our server (update source on the node backend / Peggy)
  - backend: `GET /firmware/manifest` + `GET /firmware/<sha256>.bin` behind the node auth of `/turn`
    (bearer + `X-Node-Credential`), `vesper-node firmware publish|status|withdraw`
    (`backend/src/vesper_node/firmware.py`, `tests/test_firmware.py`);
  - node: update check in `firmware/hatch/vesper_ota.c` + `muse_chat_vesper.c` (boot + 6 h + jitter,
    `>ota.check`, https only, strictly newer only); install through the kept `ota.c`
    (`sdk-patches/0005`: `ota_start_request` with exact version/size/SHA-256 checks before the boot
    partition switches); `app.c` keeps a fresh image only once the update server answered with the
    node's credential, else rollback; version from `firmware/hatch/VERSION`;
  - host checks: `make -C firmware test` (`test_vesper_ota`), `make -C firmware live-ota`
    (also with the real signed build), `make -C backend verify`. Wire: `docs/node-wire-protocol.md`
    *Firmware updates*; firmware: `firmware/README.md` *Firmware updates (task 13, F4)*.
- [x] Fleet notes: per-node firmware identical except NVS identity; N nodes = N registry rows, N claim ceremonies, one backend
  - `firmware/README.md`, *Fleet notes (task 13)*: identity, the per-node procedure, how to roll an
    update to the fleet and stop one.

## Definition of done
- [ ] An OTA update served by our backend is applied on the device and the node comes back claimed and working
> blocked: HUMAN GATE (claim + backend restart). The board was USB-flashed with the task-13 build
> (`1.0.0`, fresh `b1a3822` worktree via `apply-sdk.sh`, 2,035,712 bytes) and boots, reports
> `firmware 1.0.0` / `update: not_checked`, and accepts `>ota.check`. It is **unclaimed** (the live
> registry has no nodes) and the live `com.vesper.node` still runs pre-task-08 code (`/claim/start`
> 404), so no update check can run on it: claiming needs a person to read the code off the screen
> (task 11's gate). Everything off the device is verified (`make -C firmware live-ota` with the real
> signed 1.0.0 and 1.0.1 builds: manifest, download, exact size, SHA-256, no downgrade, refused
> credential). The 1.0.1 test image is ready at
> `~/builds/muse-charm/scratch/ota-test/muse-gadget-1.0.1.bin`. Kevin's steps:
> `firmware/README.md`, *On-device check (task 13: an OTA served by our backend, human gate)*:
> restart `com.vesper.node` from main, set `>hatch.host=https://peggy.fly.dev/vesper-node`, claim
> the code on the screen, `vesper-node firmware publish .../muse-gadget-1.0.1.bin`, `>ota.check`,
> then confirm `Version: 1.0.1`, `OTA image validated (Wi-Fi up, update server answered)`,
> `"credential":true,"claim":"claimed"` and a spoken push-to-talk reply.

## Anti-deliverables (do NOT build in this task)
- Multi-room expansion itself (comes after this in the suggested order)

## Risks / unknowns
- HUMAN GATE: device in hand for the first OTA test

## Notes for Claude Code
- Start by reading the linked `specs/` files and, if needed, the original source doc.
- Tick off boxes in this file as you complete them.
- If a deliverable is blocked, append `> blocked: <reason>` under it rather than removing it.
- Commit incrementally; one logical change per commit.
