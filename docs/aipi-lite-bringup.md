# AiPi Lite bring-up runbook

Flash-day runbook for the red AIPI Lite (X-Origin, ESP32-S3, 128x128 LCD,
mic + speaker, magnetic case + 450 mAh battery module). Hardware arrives
**Thursday 2026-10-08**. Everything up to and including the build was run on
this Mac on 2026-10-03; everything after "plug in the board" is **derived from
SDK source and not yet observed** (labelled `EXPECTED`).

SDK: `facebookincubator/muse-gadget-sdk` @ `b1a3822`, cloned (unmodified, outside
this repo) at `~/builds/muse-charm/scratch/muse-gadget-sdk`. All SDK paths below
are relative to its `esp32/` directory unless noted.

## Human gates (three, none can be done by an agent)

1. **Kevin fetches the SDK token** from <https://gadgets.muse.ai/settings/sdk-tokens>
   (Account > SDK tokens). Never paste it into chat, a commit, a log or a file in
   this repo.
2. **Kevin plugs the board in** over USB-C on flash day (data cable).
3. **Kevin pairs in the Muse app** on his phone: Settings > Devices > Developer
   mode ON, then Settings > Devices > Add Device (the `+`), then presses the
   board's talk button to confirm.

## What's in the box

- AIPI Lite device (two buttons on the bottom edge, 128x128 LCD, one mic, speaker).
- 450 mAh battery module, quick-start guide.
- **NOT included: USB-C cable and power adapter.** Bring a **data-capable**
  USB-C cable. A charge-only cable shows up as nothing at all in `ls /dev/cu.usb*`
  and is the most likely first-hour failure.

Buttons (`components/muse/boards/board_aipi.c`, `components/muse/muse_input.c`):

| Button | GPIO | Role |
|---|---|---|
| bottom right | 42 | talk (push-to-talk); Select while the menu is open |
| bottom left | 1 | wakes the board; steps down the on-screen menu (no touch) |

GPIO10 is the battery-switch latch: firmware drives it high at boot to stay
powered, and drops it for "power off" (`board_aipi.c`). The chip's own USB
Serial/JTAG (VID:PID `303A:1001`) is the console and flashing port; no vendor
bridge chip, so **no macOS USB serial driver is needed**.

## Prerequisites (validated on this host)

| Item | Value |
|---|---|
| Host | macOS arm64 (Darwin 27.0.0), 16 cores, Python 3.14.6 |
| ESP-IDF | **v6.0.1** (only supported version, `AGENTS.md`), at `~/esp/esp-idf-v6.0.1` |
| IDF Python env | `~/.espressif/python_env/idf6.0_py3.14_env` |
| Toolchain | `xtensa-esp-elf` esp-15.2.0_20251204 (GCC 15.2.0), in `~/.espressif/tools` |
| esptool | 5.4.0 (installed by IDF; flashing needs no separate install) |
| SDKROOT | `export SDKROOT=$(xcrun --sdk macosx --show-sdk-path)` was set (see simulator-setup.md for the Xcode 26.2 vs CLT SDK mismatch); cheap insurance for host-side tools |

Install (one time, already done here, about 8 min mostly download):

```sh
mkdir -p ~/esp && cd ~/esp && git clone -b v6.0.1 --recursive --depth 1 --shallow-submodules https://github.com/espressif/esp-idf.git esp-idf-v6.0.1
cd ~/esp/esp-idf-v6.0.1 && ./install.sh esp32s3
```

`board.sh` auto-finds `~/esp/esp-idf-v6.0.1/export.sh` (or set `IDF_EXPORT=<path to export.sh>`).
For interactive `idf.py` / `monitor`, activate it in each new terminal:
`. ~/esp/esp-idf-v6.0.1/export.sh`.

## SDK token: exact flow (from source)

Pinned down from `esp32/README.md` (What you need, step 2), `esp32/AGENTS.md`
(Build), `main/Kconfig.projbuild` (`GADGET_SDK_TOKEN`), `cmake/validate_config.cmake`
and `main/app.c` (boot banner). Correcting the HANDOVER shorthand:

- **Where it comes from:** gadgets.muse.ai > Account > SDK tokens.
- **Format:** `mgst_` + 43 canonical base64url characters = **48 characters**.
  `validate_config.cmake` fails the build (`FATAL_ERROR ... not a valid SDK token`)
  if the length is not 48 or it does not match `^mgst_[A-Za-z0-9_-]*[AEIMQUYcgkosw048]$`.
- **Where it goes: build-time Kconfig**, `CONFIG_GADGET_SDK_TOKEN`
  (menuconfig: ESP32 Device SDK > Muse Gadgets SDK token). It is compiled into the
  firmware image (README: "Your SDK token ships inside the firmware"). It is NOT
  entered over BLE and NOT typed in the phone app.
- **What the device does with it:** hands it to the Muse app inside the encrypted
  pairing session, and includes it when refreshing its device token
  (`Kconfig.projbuild` help text). Pairing needs it ("Gadgets without one will stop pairing").
- **Device token is a different thing:** minted by the app + BLE pairing of the
  real board, stored in the board's NVS. The SDK token alone is rejected (401) by
  the VM API (task 02 finding).
- **Build without a token works.** Verified: the build completes, with a CMake
  warning `No SDK token: set CONFIG_GADGET_SDK_TOKEN ... Gadgets without one will
  stop pairing.` So the validated build below is token-less and **not suitable for
  pairing**; flash day needs a rebuild with the token.
- The firmware logs the first 12 characters of the token at three sites: the boot
  banner (`main/app.c`), pairing confirmation (`main/link_pairing.c`) and every
  device-token refresh (`main/vm_api.c`). So serial logs leak a prefix anywhere,
  not only at boot; redact every `mgst_` before sharing a log (command in the
  Monitor section).
- Don't commit it: the generated `build-muse-aipi/sdkconfig` is where it lives
  (gitignored in the SDK; this repo contains no builds). If it leaks, revoke on
  gadgets.muse.ai, mint a new one and rebuild.

### Setting the token (flash day, HUMAN GATE 1)

Use menuconfig against the same build dir and defaults `board.sh` uses, so the
value lands in `build-muse-aipi/sdkconfig`, which `board.sh` reuses (it only
deletes `managed_components/` and `dependencies.lock`, not the build dir):

```sh
cd ~/builds/muse-charm/scratch/muse-gadget-sdk/esp32
. ~/esp/esp-idf-v6.0.1/export.sh
idf.py -B build-muse-aipi -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-muse-aipi/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-aipi" menuconfig
```

In menuconfig: ESP32 Device SDK > Muse Gadgets SDK token, paste, save, quit.
(`CONFIG_GADGET_SDK_TOKEN=` is the only line to change. Do not `cat`/`grep` it
afterwards in a shared terminal.) Optional while iterating: set
`CONFIG_HOMEHUB_WIFI_SSID` / `CONFIG_HOMEHUB_WIFI_PASSWORD` in the same menu to
skip BLE Wi-Fi provisioning; the board still needs one pairing for its device
token (`AGENTS.md`, First boot and pairing). Those are secrets too: keep them out of git.

AGENTS.md note: once `sdkconfig` exists it overrides later edits to
`sdkconfig.defaults`/overlays; after changing those, delete `build-muse-aipi/`
and rebuild. That also deletes the token, so re-enter it.

## Build

Needs no board and no token. One command, from the SDK `esp32/` directory:

```sh
export SDKROOT=$(xcrun --sdk macosx --show-sdk-path)
cd ~/builds/muse-charm/scratch/muse-gadget-sdk/esp32
tools/muse/board.sh build aipi
```

What `board.sh` runs (`tools/muse/board.sh`): `idf.py -B build-muse-aipi
-DIDF_TARGET=esp32s3 -DSDKCONFIG=build-muse-aipi/sdkconfig
-DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-aipi" build`,
log to `/tmp/muse_build_aipi.log`; it removes `managed_components/` and
`dependencies.lock` before and after (build one board at a time).

**Observed 2026-10-03 (clean, first build, no board, no token):**

- Exit 0. Wall time **71 s** (2180 ninja steps, 16 cores) after the first-time
  component download (the IDF component manager fetched `espressif/led_strip`
  and the other managed components in the same run).
- Compiler: `GNU 15.2.0`, `xtensa-esp32s3-elf-gcc`. IDF `6.0.1`.
- Expected warning in the log (benign): `CMake Warning at cmake/validate_config.cmake:40 ... No SDK token`.
  Other CMake notes about "user-set value" Kconfig options and "Missing kconfig option.
  Re-run the build process..." are normal on a fresh `sdkconfig`.
- Console prints nothing from `grep "error:|undefined reference"`, then the
  last three log lines:

```
 python -m esptool --chip esp32s3 -b 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size keep --flash-freq 80m 0x0 build-muse-aipi/bootloader/bootloader.bin 0x10000 build-muse-aipi/partition_table/partition-table.bin 0x1d000 build-muse-aipi/ota_data_initial.bin 0x20000 build-muse-aipi/muse-gadget.bin
or from the ".../esp32/build-muse-aipi" directory
 python -m esptool --chip esp32s3 -b 460800 --before default-reset --after hard-reset write-flash "@flash_args"
```

- Earlier in `/tmp/muse_build_aipi.log`:
  `muse-gadget.bin binary size 0x211000 bytes. Smallest app partition is 0x400000 bytes. 0x1ef000 bytes (48%) free.`
  then `Project build complete.`
- Artifacts (stay in the SDK dir, never copied into this repo):
  `build-muse-aipi/muse-gadget.bin` (signed with the SDK's committed
  `dev_signing_key.pem`; Secure Boot is NOT enabled so nothing is burnt to eFuse),
  `bootloader/bootloader.bin`, `partition_table/partition-table.bin`, `ota_data_initial.bin`.
  Flash layout (`flash_args`): `0x0` bootloader, `0x10000` partition table,
  `0x1d000` otadata, `0x20000` app.
- Rebuild after a token change is incremental and quick.

Simulator check after the build: `ctest` in `esp32/simulator/build` still passes
(1/1, `muse_simulator_headless`, 0.70 s).

## Flash (HUMAN GATE 2: plug in)

1. Plug the board into the Mac with the **data** USB-C cable. Slide/press the
   battery switch per the quick-start guide so it powers on (USB alone also powers it).
2. Find the port. The SDK finds boards by USB ID, not port name:

```sh
cd ~/builds/muse-charm/scratch/muse-gadget-sdk/esp32
python tools/muse/ports.py --list      # lists boards on USB; needs pyserial (IDF env has it)
python tools/muse/ports.py aipi        # prints the port for the AIPI
ls /dev/cu.usbmodem*                   # fallback; expect one entry, e.g. /dev/cu.usbmodem1101
```

   `ports.py aipi` matches VID:PID `303A:1001` (Espressif USB Serial/JTAG). If
   several matching boards are attached pass the USB serial number (the MAC) or the port.
3. Flash (after the token has been set and the board built):

```sh
tools/muse/board.sh build aipi && tools/muse/board.sh flash aipi
# or an explicit port: tools/muse/board.sh flash aipi /dev/cu.usbmodem1101
```

   `flash` runs, from `build-muse-aipi/`, `python -m esptool --chip esp32s3 -p PORT -b 460800
   --before default-reset --after hard-reset write-flash "@flash_args"` and prints only
   the last 3 lines. Do the sequence with the IDF env active (`board.sh` does this itself).
   Note `board.sh build` clears `managed_components/` each time; the built
   `sdkconfig` and token in `build-muse-aipi/` are kept.
4. To watch progress, run the esptool line above directly from `build-muse-aipi/`.

**EXPECTED, not observed** (esptool 5.x output shape): `Connected to ESP32-S3 on
/dev/cu.usbmodemXXXX`, `Chip type: ESP32-S3 (revision ...)`, `Features: ... Embedded
PSRAM 8MB (AP_3v3)`, `Flash size: 16MB` (confirms the overlay assumptions: 8 MB
octal PSRAM, 16 MB flash), per-region `Writing at 0x... [====] 100%`, `Hash of data verified.`,
and final `Hard resetting via RTS pin...` (board.sh's `tail -3` shows roughly these last lines).

If esptool cannot connect, see Troubleshooting (download mode).

## Monitor / first boot (EXPECTED, derived from source)

```sh
. ~/esp/esp-idf-v6.0.1/export.sh
cd ~/builds/muse-charm/scratch/muse-gadget-sdk/esp32
idf.py -B build-muse-aipi -p "$(python tools/muse/ports.py aipi)" monitor      # Ctrl-] quits; resets the board
# build-dir-independent alternative (resets the board, prints N seconds of log, exits):
python tools/muse/monitor.py "$(python tools/muse/ports.py aipi)" 15
```

**`-B build-muse-aipi` is mandatory on every `idf.py` command in this runbook.**
The SDK's top-level `sdkconfig.defaults` sets `CONFIG_IDF_TARGET="esp32c5"`, and
there is no `esp32/build/` or `esp32/sdkconfig`. A bare `idf.py -p PORT monitor`
(or `erase-flash`) therefore configures a fresh default `esp32/build/` for the
wrong chip (ESP32-C5, not the AIPI's ESP32-S3), writes a stray `esp32/sdkconfig`, and then
fails or talks to the board with the wrong target. With `-B build-muse-aipi` idf.py
reuses that build's cached config (verified 2026-10-03: it resolves `--target esp32s3`
and `build-muse-aipi/muse-gadget.elf`, no stray files). If you forget it, delete
`esp32/build/`, `esp32/sdkconfig` (and `esp32/sdkconfig.old` if present), then
re-run with `-B build-muse-aipi`. `tools/muse/monitor.py PORT [secs]` (default 8 s,
115200 baud) needs no build dir at all.

**Serial output contains an SDK-token prefix.** The firmware logs the first 12
characters of the `mgst_` SDK token at three places (`%.12s` in source):

| Source | Log line | When |
|---|---|---|
| `main/app.c` (~2495) | `  SDK token:  mgst_xxxxxxx` | every boot (banner) |
| `main/link_pairing.c` (~1051) | `pairing_confirmed carries SDK token mgst_xxxxxxx` | pairing confirmation |
| `main/vm_api.c` (~425) | `refresh carries SDK token mgst_xxxxxxx` | every device-token refresh |

Treat **any** `mgst_` occurrence in serial output as sensitive, not only the boot
banner. Before sharing a log anywhere (chat, issue, commit), save it to a file
outside the repo and redact it, then share only the redacted file:

```sh
python tools/muse/monitor.py "$(python tools/muse/ports.py aipi)" 20 > ~/boot.log
sed -E 's/mgst_[A-Za-z0-9_-]+/mgst_REDACTED/g' ~/boot.log > ~/boot.redacted.log
grep -oE 'mgst_[A-Za-z0-9_-]+' ~/boot.redacted.log | sort -u   # must print only: mgst_REDACTED
```

Delete `~/boot.log` once done (it holds the unredacted prefix).

A healthy boot (`AGENTS.md` Monitor section; `main/main.c`, `main/app.c`):

```
I (...) link.main: Muse Gadget starting
...
I (...) <tag>: ====== Muse Gadget ======
I (...) <tag>:   Node:     ...
I (...) <tag>:   Version:  999.0.0
I (...) <tag>:   SDK token:  mgst_xxxxxxx          <- first 12 chars only; "(none)" if no token
I (...) <tag>: ========================
I (...) <tag>: BLE advertising; press the button to confirm pairing
```

On screen (128x128, `AGENTS.md` pairing section): avatar; unpaired, the 128 px
screen omits the `MuseGadget-XXXXXX` name and status (so identify the board in
the app by its advertised name `MuseGadget-XXXXXX`, visible in the app list
or the serial log). Heartbeat log every 5 s (`main/app.c`) shows where it is stuck.
Version `999.0.0` is the SDK default build version.

## Pair (HUMAN GATE 3: Kevin's phone)

Per `esp32/README.md` step 4 / `AGENTS.md`:

1. Board shows it is in setup (SDK colours: orange breathing = BLE advertising, waiting for
   setup; on the AIPI that is shown by the avatar/caption, not an LED).
2. Muse app > **Settings > Devices > Developer mode** ON (needed to see community devices;
   community pairing v5 requires a recent app).
3. Settings > Devices > **Add Device** (`+`, top right). It appears as `MuseGadget-XXXXXX`. Pick it.
4. When it asks for confirmation, **press the board's talk button (bottom right)**
   ("short press: confirm a pending pairing"). Joins Wi-Fi (provisioned over BLE by the app),
   then connects to Muse. Connected = green in SDK terms; the AIPI shows mic icon + normal avatar
   (the mic icon appears only once paired).
5. Community pairing has no manufacturer attestation and does not prevent an active
   MITM: pair on a trusted network.

To restart setup: bottom-left opens the on-board menu (touch-less board), step to
`Phone setup` (Toggle) or `Reset pairing` (Select), confirm with bottom right. The README's
"hold the button 5 s to reset" is for BOOT-button dev boards. The double-press and
hold-to-power-off gestures in `muse_input.c` (`aux_button`) apply to touch boards only;
the AIPI's aux button goes through `menu_button`. Powering off on the AIPI is menu > `Power off`.

## Verify (EXPECTED)

- Avatar renders on the 128x128 LCD, idle animation; backlight on.
- Hold bottom right: listening state (mic), release: thinking, then speaking with
  caption and audio from the speaker (ES8311 codec, PA enable GPIO9).
- Bottom left: opens the menu; each press steps down; bottom right selects. Items
  (`components/muse/muse_menu.c`): Volume, Speaker, Brightness, Mic gain, Auto-sleep, Phone
  setup, Wi-Fi, Status, Battery, Reset pairing, Screen off, Power off, Close menu.
- First voice round-trip: say a short sentence, expect reply audio within a few seconds.
- Console helper: `python tools/muse/chat.py --status` (board status) and
  `python tools/muse/chat.py "question"`.
- On battery the screen dims then the CPU light-sleeps between button polls
  (`CONFIG_PM_ENABLE` in `devices/sdkconfig.muse-aipi`); only buttons wake it.
  The board powers off via menu > Power off (GPIO10 latch released, deep sleep).
- Reflashing keeps pairing and Wi-Fi (NVS untouched).

## Troubleshooting

| Symptom | Likely cause / action |
|---|---|
| `ports.py aipi` or `ls /dev/cu.usb*` finds nothing | Charge-only cable (swap), board not powered (battery switch), or the stock firmware has USB-JTAG off. Try another port/cable; `system_profiler SPUSBDataType` should show "USB JTAG/serial debug unit". |
| esptool "Failed to connect" / "No serial data received" | Put the ESP32-S3 in download mode (hold the BOOT/GPIO0 strapping button while plugging in USB, per the quick-start guide; AIPI Lite's GPIO0 access is **unverified** until the hardware arrives). The AIPI's two user buttons are GPIO42/GPIO1, neither is BOOT. Then re-run flash with `--after hard-reset` and power-cycle. **Fallback if no BOOT/GPIO0 button is reachable (unverified on the AIPI):** the ESP32-S3's built-in USB-Serial/JTAG normally enters download mode on its own via esptool's reset sequence, so no button should be needed. From `build-muse-aipi/`, retry with the USB reset and a lower baud: `python -m esptool --chip esp32s3 -p PORT -b 115200 --before usb-reset --after hard-reset write-flash "@flash_args"` (`usb-reset` is a valid `--before` choice in esptool 5.4.0). If that fails too, unplug, switch the battery off, replug and retry straight away. |
| Port disappears after flash | Normal: the chip resets and USB re-enumerates, possibly under a new `/dev/cu.usbmodem*` name. Re-run `ports.py aipi`. |
| Build: stale or wrong config | Delete `build-muse-aipi/` (and re-enter the token), rebuild. `validate_config.cmake` stops the build if config is stale. |
| Build: component manager / lvgl error | `rm -rf managed_components dependencies.lock` and rebuild (`board.sh` does this itself). Build one board at a time. |
| Build: `CONFIG_GADGET_SDK_TOKEN is not a valid SDK token` | Wrong length (must be 48 chars) or typo; copy again from gadgets.muse.ai. |
| Build: missing `gcm.h` / `gpio_ll.h` | Wrong IDF version; use v6.0.1. |
| Host compile fails with ld/tapi `arm64e` errors | Xcode vs CLT SDK mismatch: `export SDKROOT=$(xcrun --sdk macosx --show-sdk-path)`. |
| Boot loop (repeated resets / panic) | Capture with `tools/muse/monitor.py PORT 20`. Often missing PSRAM in config (overlay requires it; `CONFIG_SPIRAM_IGNORE_NOTFOUND` is off, so no PSRAM = abort at boot) or a stale sdkconfig. Do a clean rebuild + erase-flash (below). |
| Never pairs / purple state | Built without a token, or the SDK token was revoked. Rebuild with a valid token; pair again. |
| No Wi-Fi | 2.4 GHz only; re-provision via the app, or reset pairing (menu > Reset pairing) and pair again. |
| No sound | Check Volume/Speaker in the on-board menu; check `PA_EN` (GPIO9) / codec init lines in the log. |
| Board won't stay on battery | The firmware must hold the GPIO10 latch; check the battery module is seated and charged (charge pin GPIO8). |

**Clean re-flash** (wipes pairing, Wi-Fi and everything else in flash):

```sh
. ~/esp/esp-idf-v6.0.1/export.sh
cd ~/builds/muse-charm/scratch/muse-gadget-sdk/esp32
idf.py -B build-muse-aipi -p "$(python tools/muse/ports.py aipi)" erase-flash   # -B is mandatory (see Monitor)
tools/muse/board.sh flash aipi
```

Do not enable Secure Boot, flash encryption or `CONFIG_HOMEHUB_PAIRING_EFUSE_AUTH`
(burns eFuses; `AGENTS.md`). Returning to the factory firmware: this runbook does not
cover it; back up the stock flash first if wanted (`python -m esptool --chip esp32s3 -p PORT read-flash 0 0x1000000 stock.bin`, 16 MB, off-repo).

## Flash-day checklist

- [ ] Have: **data** USB-C cable, phone with the Muse app, trusted 2.4 GHz Wi-Fi, charged battery module.
- [ ] (Optional, recommended) back up stock flash to a file outside the repo.
- [ ] **GATE 1:** fetch the SDK token from gadgets.muse.ai (Account > SDK tokens). Do not paste it anywhere else.
- [ ] `. ~/esp/esp-idf-v6.0.1/export.sh && export SDKROOT=$(xcrun --sdk macosx --show-sdk-path)`
- [ ] `cd ~/builds/muse-charm/scratch/muse-gadget-sdk/esp32`
- [ ] Run the menuconfig command above, set Muse Gadgets SDK token, save.
- [ ] `tools/muse/board.sh build aipi` (expect exit 0, ~1-2 min, `Project build complete`; token warning gone).
- [ ] **GATE 2:** plug the board in with USB-C. `python tools/muse/ports.py aipi` prints a port.
- [ ] `tools/muse/board.sh flash aipi` (expect `Hash of data verified`).
- [ ] `idf.py -B build-muse-aipi -p "$(python tools/muse/ports.py aipi)" monitor` (never drop `-B build-muse-aipi`): see `link.main: Muse Gadget starting` and a `SDK token:  mgst_` prefix (not `(none)`).
- [ ] **GATE 3:** Muse app > Settings > Devices > Developer mode > Add Device > `MuseGadget-XXXXXX`; press bottom-right to confirm.
- [ ] Verify avatar, push-to-talk, first voice round trip.
- [ ] Anything odd: capture the log to a file outside the repo, redact with `sed -E 's/mgst_[A-Za-z0-9_-]+/mgst_REDACTED/g' ~/boot.log > ~/boot.redacted.log` and share only `boot.redacted.log` (any `mgst_` is sensitive: banner, pairing and refresh lines all carry it); use the troubleshooting table; last resort clean re-flash.
