# USB Flash Runbook — Vesper Node (AiPi Lite)

## When to use
Board OTA is broken (board provisioned with HTTP `hatch.host=http://192.168.5.16:8797`;
firmware requires HTTPS for updates — every OTA check is silently skipped as `needs_https`).
Until `hatch.host` is fixed to the HTTPS Peggy URL, firmware updates need USB flashing.
After step 3 below, future OTAs work again.

## Prerequisites
- AiPi Lite board + USB data cable, plugged into the Studio (mac-daddy31337).
- ESP-IDF environment (the `esptool` in the IDF Python env). The Studio has it at
  `~/esp/esp-idf-v6.0.1` (`. ~/esp/esp-idf-v6.0.1/export.sh`; `board.sh` sources the same).

## The binary: firmware 1.0.4 ("Hey Vesper" wake word, task 22)
- Built from a fresh `b1a3822` tree with `firmware/apply-sdk.sh` (patches 0001-0008) and
  `tools/muse/board.sh build aipi`, ESP-IDF v6.0.1.
- Build dir: `/Users/k3v/builds/muse-charm/scratch/sdk-impl-22/esp32/build-muse-aipi/`
- Durable copy (survives that tree being rebuilt):
  `/Users/k3v/builds/muse-charm/scratch/firmware-1.0.4/` — `muse-gadget.bin`, `app-flash_args`,
  `flash_args`, `muse-gadget.bin.sha256`.
- `muse-gadget.bin`: 2,297,856 bytes (`0x231000`, 45% of the 4 MiB app slot free), version
  string `1.0.4`, sha256 `56dabbd6ec0a7ea3240da16f382b9f491c6b4086710d25a10c71a82038549cda`.
  It contains the 60,840-byte "Hey Vesper" microWakeWord model; the "Computer" WakeNet of
  1.0.3 is gone (see `firmware/README.md`, *Wake word*).

## Steps

### 1. Find the USB port
```sh
ls /dev/cu.usbmodem*
```
The port re-enumerates on each plug; it was `/dev/cu.usbmodem83201` on 2026-10-07.

### 2. Flash firmware 1.0.4 ("Hey Vesper") — app only
The board is already provisioned (Wi-Fi, NVS credential). Flash ONLY the app
partition — do NOT rewrite bootloader/partition-table/NVS. Same partition table as 1.0.3.
```sh
cd /Users/k3v/builds/muse-charm/scratch/firmware-1.0.4
shasum -a 256 -c muse-gadget.bin.sha256
python -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXXX -b 460800 \
  --before default-reset --after hard-reset write-flash "@app-flash_args"
```
Replace XXXX with the actual port suffix. (`app-flash_args` is `0x20000 muse-gadget.bin`; the
same command works from the build dir above.)

### 3. Fix hatch.host so future OTAs work (serial, 115200 baud)
Open a serial monitor on the same port (e.g. `idf.py -p /dev/cu.usbmodemXXXX monitor`),
then type at the `>` prompt:
```
>hatch.host=https://peggy.fly.dev/vesper-node
```
This replaces the broken `http://192.168.5.16:8797` socat bridge. The firmware's
OTA gate is HTTPS-only — with the Peggy URL, boot-time and 6-hourly checks work.

### 4. Verify
- Board boots, shows the Iconic face.
- `>status` via serial shows version 1.0.4 and `"wake":{"state":"on","model":"hey_vesper",...}`.
- The boot log has `vesper_wake: wake word on: "Hey Vesper" ...` and a `wake word memory: ...`
  line: note the inference time and arena figures (firmware/README.md, *On-device check
  (task 22)*, step 2).
- Push-to-talk still works with voice reply.
- Say "Hey Vesper", pause, ask something — the board wakes to listening state, then replies.
- Say "Hey Vesper" alone — 5 s of LISTENING, then idle, no backend `/turn`.
- If false wakes are a nuisance: `>wake.threshold=0.75`; if it misses you: `>wake.threshold=0.60`
  (default 0.65; saved in NVS, no restart).
- Backend log shows the board checking in (no "firmware check" needed — 1.0.4 is current).
