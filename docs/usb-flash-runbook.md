# Firmware delivery runbook: Vesper node (AiPi Lite). OTA first, USB as the fallback

## The delivery path is OTA
Since the Studio visit on 2026-10-09, the board's `hatch.host` is
`https://peggy.fly.dev/vesper-node`. Its boot log shows
`update check: up to date (HTTP 200; running 1.0.2, published 1.0.2)`. Firmware now reaches the
board **over the air**: publish a build on the Studio, and the board installs it at its next
check. That check runs 10 s after boot, every 6 hours after that, or on serial `>ota.check`.
USB flashing (below) is only a fallback.

## The binary: firmware 1.0.4 ("Hey Vesper" wake word, task 22)
- Built from a fresh `b1a3822` tree with `firmware/apply-sdk.sh` (patches 0001-0008) and
  `tools/muse/board.sh build aipi`, using ESP-IDF v6.0.1.
- Build dir: `/Users/k3v/builds/muse-charm/scratch/sdk-impl-22-r3/esp32/build-muse-aipi/`
- Durable copy, which survives that tree being rebuilt:
  `/Users/k3v/builds/muse-charm/scratch/firmware-1.0.4/`. It holds `muse-gadget.bin`,
  `app-flash_args`, `flash_args` and `muse-gadget.bin.sha256`.
- `muse-gadget.bin` is 2,297,856 bytes (`0x231000`; 45% of the 4 MiB app slot stays free). Its
  version string is `1.0.4`, project `muse-gadget`, chip ESP32-S3, and its sha256 is
  `c2f976258dc2478fb1bfa5b2afb8f654cededfabeffd04902d5a152ff1d0a682`.
- It contains the 60,840-byte "Hey Vesper" microWakeWord model. 1.0.3's "Computer" WakeNet
  is gone: 1.0.3 crash-looped on the board, and 1.0.3 was never published. See
  `firmware/README.md`, *Wake word*, *Memory budget and the 1.0.3 crash*.

## A. OTA: publish after the merge
Run this on the Studio, from the main checkout, **after the task-22 branch is merged**. The
`/implement` orchestrator does this step; the implementer doesn't.

```sh
cd /Users/k3v/builds/muse-charm/muse-charm/backend
(cd /Users/k3v/builds/muse-charm/scratch/firmware-1.0.4 && shasum -a 256 -c muse-gadget.bin.sha256)
uv run --locked vesper-node firmware publish /Users/k3v/builds/muse-charm/scratch/firmware-1.0.4/muse-gadget.bin
uv run --locked vesper-node firmware status
```

- **What the CLI takes:** the app image `muse-gadget.bin`. That is not `flash_args` and not the
  bootloader. Before it stores anything, `publish` checks the image: chip ESP32-S3, project
  `muse-gadget`, version `MAJOR.MINOR.PATCH`, at most 4 MiB, and newer than the published
  version. The published version is 1.0.2, so no `--force` is needed. The image goes into the
  backend's firmware store (`~/.config/vesper-voice/firmware/` unless `node.env` sets another),
  and the running `com.vesper.node` service serves it. It prints
  `published: version 1.0.4, 2297856 bytes, sha256 c2f97625...`.
- **Watch the rollout:** the backend log shows the board's check and download
  (`firmware check: node=... running=1.0.2 published=1.0.4`, then the image request). After the
  reboot, `running=1.0.4` appears. To make the board check now, type `>ota.check` on its serial
  console; otherwise it checks within 6 h or at its next boot.
- **Verify on the board:** the boot log says
  `OTA image validated (Wi-Fi up, update server answered)` and shows the `vesper_wake: wake word
  on: "Hey Vesper" ...` and `wake word memory: ...` lines. `>status` shows version 1.0.4 and
  `"wake":{"state":"on","model":"hey_vesper",...}`. Then run the checks in section C.
- **If 1.0.4 crashes before it is validated**, the board goes back to 1.0.2 by itself.
  "Validated" means Wi-Fi is up and the backend has answered an update check, at least 10 s
  after boot. The bootloader has app rollback on. An unvalidated image that panics or reboots
  is aborted and the previous slot boots. The backend keeps seeing `running=1.0.2`. 1.0.2's boot
  log says `rolled back here before: 1.0.4` and it won't install 1.0.4 again. Withdraw the
  release (`uv run --locked vesper-node firmware withdraw`) and report back. Details:
  `firmware/README.md`, *Memory budget and the 1.0.3 crash*.
- **If 1.0.4 crashes only after it is validated** (on a wake or a turn), it doesn't roll back.
  First try `>wake=off` on serial: it is kept in NVS, and push to talk keeps working. If that
  doesn't hold, publish a fixed 1.0.5 (nodes refuse a downgrade, so re-publishing 1.0.2 won't
  install), or use the USB fallback (B) with the 1.0.2 image.

## B. USB fallback
Use this only when OTA can't work: the board can't reach the backend, or `hatch.host` is wrong
again (an `http://` host is refused, and every check is skipped as `needs_https`). Also use it
when a validated image misbehaves and OTA can't replace it.

### Prerequisites
- The AiPi Lite board and a USB data cable, plugged into the Studio (mac-daddy31337).
- The ESP-IDF environment, for its `esptool`. The Studio has it at `~/esp/esp-idf-v6.0.1`
  (`. ~/esp/esp-idf-v6.0.1/export.sh`; `board.sh` sources the same).

### 1. Find the USB port
```sh
ls /dev/cu.usbmodem*
```
The port re-enumerates on each plug; it was `/dev/cu.usbmodem83201` on 2026-10-07.

### 2. Flash firmware 1.0.4: app partition only
The board is already provisioned (Wi-Fi, NVS credential). Flash ONLY the app. Do NOT rewrite
the bootloader, the partition table or NVS. The partition table is the same as 1.0.2's and
1.0.3's.
```sh
cd /Users/k3v/builds/muse-charm/scratch/firmware-1.0.4
shasum -a 256 -c muse-gadget.bin.sha256
python -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXXX -b 460800 --before default-reset --after hard-reset write-flash "@app-flash_args"
```
Replace XXXX with the actual port suffix. `app-flash_args` writes `0x20000 muse-gadget.bin`
(slot `ota_0`), and the same command works from the build dir above.

**Slot caveat:** after an OTA, the board may be booting from `ota_1` (`0x420000`). An app-only
write to `ota_0` is then ignored on boot. If the boot log's version doesn't change, also reset
the OTA selection back to `ota_0`. This doesn't touch NVS; it does clear the rollback history:
```sh
python -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXXX -b 460800 --before default-reset --after hard-reset write-flash 0x1d000 /Users/k3v/builds/muse-charm/scratch/sdk-impl-22-r3/esp32/build-muse-aipi/ota_data_initial.bin 0x20000 /Users/k3v/builds/muse-charm/scratch/firmware-1.0.4/muse-gadget.bin
```
A USB-flashed image never boots as `PENDING_VERIFY`, so **the bootloader won't roll it back**.
That is why 1.0.3, flashed by USB, boot-looped instead of reverting. Prefer OTA (A) for any
build that hasn't run on the board yet.

### 3. Check `hatch.host` (serial, 115200 baud)
Open a serial monitor on the same port (e.g. `idf.py -p /dev/cu.usbmodemXXXX monitor`). If the
boot log doesn't show `update check: ... HTTP 200`, set the host at the `>` prompt:
```
>hatch.host=https://peggy.fly.dev/vesper-node
```
The firmware's update check only works over HTTPS. With the Peggy URL, the boot-time and
6-hourly checks work.

## C. Verify (either path)
- The board boots and shows the Iconic face.
- `>status` on serial shows version 1.0.4 and `"wake":{"state":"on","model":"hey_vesper",...}`.
- The boot log has `vesper_wake: wake word on: "Hey Vesper" ...` and a `wake word memory: ...`
  line. Note the inference time, the arena (`arena X of 40960 B`: the real S3 figure, as the
  host's 24,608 B uses reference kernels without esp-nn's conv scratch) and the free internal heap figures
  (`firmware/README.md`, *On-device check (task 22)*, step 2).
- Push to talk still works, with a voice reply.
- Say "Hey Vesper", pause, ask something: the board goes to the listening state, then replies.
- Say "Hey Vesper" alone: 5 s of LISTENING, then idle, and no backend `/turn`.
- If false wakes are a nuisance, `>wake.threshold=0.75`; if it misses you,
  `>wake.threshold=0.60`. The default is 0.65; the value is saved in NVS and needs no restart.
- After a few turns, `>wake` shows `"heap":{"int":...,"int_min":...}`. `int_min` is the internal
  RAM low-water mark: record it.
- The backend log shows the board checking in with `running=1.0.4`.
