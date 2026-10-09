# USB Flash Runbook — Vesper Node (AiPi Lite)

## When to use
Board OTA is broken (board provisioned with HTTP `hatch.host=http://192.168.5.16:8797`;
firmware requires HTTPS for updates — every OTA check is silently skipped as `needs_https`).
Until `hatch.host` is fixed to the HTTPS Peggy URL, firmware updates need USB flashing.
After step 3 below, future OTAs work again.

## Prerequisites
- AiPi Lite board + USB data cable, plugged into the Studio (mac-daddy31337).
- ESP-IDF environment (the `esptool` in the IDF Python env). The Studio has it at
  `~/esp/v6.0.1/esp-idf` (or wherever `board.sh` sources it — `board.sh` wraps this).

## Steps

### 1. Find the USB port
```sh
ls /dev/cu.usbmodem*
```
The port re-enumerates on each plug; it was `/dev/cu.usbmodem83201` on 2026-10-07.

### 2. Flash firmware 1.0.3 ("Computer" wake word) — app only
The board is already provisioned (Wi-Fi, NVS credential). Flash ONLY the app
partition — do NOT rewrite bootloader/partition-table/NVS.
```sh
cd /Users/k3v/builds/muse-charm/scratch/sdk-impl-20/esp32/build-muse-aipi
python -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXXX -b 460800 \
  --before default-reset --after hard-reset write-flash "@app-flash_args"
```
Replace XXXX with the actual port suffix. Binary is `muse-gadget.bin`
(2,691,072 bytes, version string 1.0.3, includes the "Computer" WakeNet model).

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
- `>status` via serial shows version 1.0.3.
- Push-to-talk still works with voice reply.
- Say "Computer" — the board should wake to listening state (cyan/listening face).
- Backend log shows the board checking in (no "firmware check" needed — 1.0.3 is current).

## Later: "Hey Vesper" model swap
After task 21's model is validated, it ships as a firmware update via OTA
(once step 3 is done), replacing the "Computer" WakeNet model. No USB needed.
