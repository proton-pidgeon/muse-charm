# Muse Gadget UI Simulator — setup guide

Source of truth: `esp32/simulator/README.md` in
[facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk).
This guide is the condensed, Mac-first version. If anything here conflicts
with the upstream README, the upstream README wins.

## What it is

A desktop preview of the Muse device interface in a fixed 412 x 412
SenseCAP Watcher window. It compiles the **production** `muse_ui.c`, state
and text code, and the avatar renderer. SDL supplies display, mouse input,
and timing; small host adapters stand in for ESP-IDF, FreeRTOS, Wi-Fi,
Bluetooth, Link, settings, and power services.

**What it does NOT do:** it does not emulate the ESP32-S3 CPU, audio
hardware, the Bluetooth radio, the Watcher's camera, memory pressure, or
power timing. Those paths still need a firmware build and final testing on
a real device.

## Prerequisites (macOS)

- Xcode command-line tools: `xcode-select --install` (may report already
  installed — that's fine)
- `brew install cmake ninja python`
- Needs CMake 3.24+, GCC/Clang with C11, Python 3.9+
- **ESP-IDF is not required.** No board, no USB cable, no SDK token.

## Build

> **macOS SDK mismatch:** if configure fails with "C compiler is not able
> to compile a simple test program" / `ld: tapi error ... unknown
> architecture arm64e.x1-macos`, your Command Line Tools SDK is newer than
> the selected Xcode's linker. Run
> `export SDKROOT=$(xcrun --sdk macosx --show-sdk-path)` first (or update
> Xcode), then delete the build dir and re-configure.

```sh
git clone https://github.com/facebookincubator/muse-gadget-sdk
cd muse-gadget-sdk
cmake -S esp32/simulator -B esp32/simulator/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build esp32/simulator/build --parallel
```

CMake uses a system SDL2 when available, otherwise downloads the pinned
SDL 2.32.10. It always fetches LVGL 9.5.0 (the production UI needs
version-specific private APIs). Needs network on the first build.

## Test (headless, no display needed)

```sh
ctest --test-dir esp32/simulator/build --output-on-failure
```

Uses SDL's dummy video driver; renders every scenario twice and checks the
framebuffer captures are identical. Green = the UI builds and renders
deterministically.

## Interactive preview (needs a logged-in desktop session)

```sh
./esp32/simulator/build/muse_simulator
```

Mouse acts as touch. Keyboard controls:

| Key | Action |
|---|---|
| F1 … F7 | Boot, idle, listening, thinking, speaking, error, off |
| H | Happy idle animation |
| Space (hold) | Listen while held, release → thinking |
| `+` / `-` | Audio level meter |
| `[` / `]` | Turn progress |
| S | Toggle sleep (click window to wake) |
| P | Save `muse-simulator.ppm` screenshot |
| Esc | Quit |

On a MacBook, hold Fn/Globe with F1–F7 unless function keys are set to
standard behavior. `./esp32/simulator/build/muse_simulator --help` lists
everything. A scenario file can pre-seed a session:

```sh
./esp32/simulator/build/muse_simulator \
  --scenario esp32/simulator/tests/scenarios/pairing.txt
```

## Headless / scripted runs (SSH, CI)

```sh
./esp32/simulator/build/muse_simulator \
  --headless \
  --scenario esp32/simulator/tests/scenarios/thinking.txt \
  --run-ms 250 \
  --screenshot thinking.ppm
```

Upstream ships scenarios only for `error`, `idle`, `listening`, `pairing`,
and `thinking`. For other states write your own file, e.g. `face=speaking`
or `face=happy` (see `--help` for all keys).

Scenario files are `key=value` per line (`face`, `caption`, `progress`,
`battery`, `wifi`, `ble`, `link`, `advance`, …). Invalid values exit
nonzero naming the bad line.

## Quick smoke test (interactive)

1. 412 x 412 `Muse Gadget Simulator` window opens.
2. F3 → listening; `+`/`-` moves the level meter.
3. Hold Space → release → thinking; progress ring animates.
4. F5 → speaking; `+`/`-` animates the mouth.
5. H → happy animation plays.
6. S → sleep; click window to wake.
7. Drag left → settings placeholder; drag right → back to avatar.
8. P → `muse-simulator.ppm` appears; Esc quits.
