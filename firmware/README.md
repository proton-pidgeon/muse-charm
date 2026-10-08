# firmware/

Vesper-specific firmware pieces on top of Meta's Muse gadget SDK at commit `b1a3822`. A
pristine clone lives at `~/builds/muse-charm/scratch/muse-gadget-sdk`. Every change to the SDK is
tracked here, and `firmware/apply-sdk.sh` turns a pristine checkout into the Vesper node firmware.

| Path | What |
|---|---|
| `apply-sdk.sh <SDK_DIR>` | Idempotent. Deletes the Meta transport, applies `sdk-patches/*.patch`, installs `hatch/` and the avatar, and checks that `main/voice.c` is unchanged |
| `sdk-patches/delete.txt` | Files the patch set removes from the SDK |
| `sdk-patches/000N-*.patch` | Edits to tracked SDK files (`git apply` format against `b1a3822`) |
| `hatch/` | The Vesper `muse_hatch_*` backend (task 09), installed as `<SDK>/esp32/components/muse/vesper/` |
| `hatch/test/` | Host tests of the protocol core, and a live-turn harness |
| `avatar/` | The Vesper owl avatar (task 04) |
| `Makefile` | `make -C firmware test` and `make -C firmware live-turn` (host only, no ESP-IDF) |

## Decision (task 09): a patch set tracked in THIS repo, not an SDK fork

The transport work follows the avatar's precedent. Nothing lives in a fork. The SDK checkout
stays a re-clonable upstream at `b1a3822`, and `apply-sdk.sh` rebuilds the Vesper tree from it:

1. **Deletions** come from a plain list (`sdk-patches/delete.txt`), not from diff hunks that
   would repeat ~10k lines of Meta source. A reviewer sees exactly which files go.
2. **Edits to tracked SDK files** are a short `git apply` series (`sdk-patches/0001-0003`):
   - build and Kconfig without `CONFIG_GADGET_SDK_TOKEN`;
   - `main/app.c` cut down to Wi-Fi, OTA validation, identity and the Muse glue;
   - the backend selection at `components/muse/CMakeLists.txt`;
   - the `muse_settings` repurpose.
3. **New Vesper-owned sources** are whole files in `hatch/`, copied into an SDK path that the
   SDK's `.gitignore` already ignores (`components/muse/vesper/`, like `components/muse/avatar/`).

Why not a fork: every change shows up in this repo's diff, where the review gate looks. The patch
set re-applies to a fresh `b1a3822` clone in one command. Upstream drift stays visible: a patch
that stops applying fails loudly instead of being merged silently. If the SDK ever needs
upstream updates, rebase the three patches.

Reproduce from scratch:

```sh
git -C ~/builds/muse-charm/scratch/muse-gadget-sdk worktree add --detach /tmp/sdk-vesper b1a3822
firmware/apply-sdk.sh /tmp/sdk-vesper
cd /tmp/sdk-vesper/esp32 && tools/muse/board.sh build aipi     # ESP-IDF v6.0.1, see docs/aipi-lite-bringup.md
```

### What the patch set removes (Meta transport)

- `main/vm_api.c`
- `main/link_pairing.c`
- `main/noise_control*`
- `main/ble_server.c`
- `components/muse/muse_account_api.c`
- `components/muse/muse_chat_session.cpp`
- `CONFIG_GADGET_SDK_TOKEN`, from Kconfig and `validate_config.cmake`

Also removed, because only that transport used it: the Noise tunnel and tunnel netif,
`net_discovery`, the pairing signer/transcript, `factory_test` (BLE), `bug_report` and
`image_fetch` (remote-control commands), `muse_chat_link.c` (the no-PSRAM backend over Link's
session), and the `noise_core` component.

The image is 1,839,104 bytes, down from 2,166,784 for the avatar-only stock build.

**Kept byte-identical:**

- `main/voice.c` (the Voice PE voice task; not even compiled for the AIPI)
- `board_aipi.c`, `voice_board.c`, `voice_player.c`
- the LVGL UI (`muse_ui.c`, `muse_settings_ui.c`, `muse_menu.c`)
- `muse_voice.c`, the AIPI's push-to-talk task, which talks to the new backend through the
  unchanged `muse_hatch_*` API
- `muse_wifi`, `identity.c`, `ota.c`, `muse_ble.c`

`apply-sdk.sh` fails if `voice.c` changes.

**The one `GADGET_SDK_TOKEN` residue:** `identity.c` must stay byte-identical, and its
`identity_sdk_token()` still names the macro. Nothing calls that function any more, so
`main/CMakeLists.txt` gives that one file `CONFIG_GADGET_SDK_TOKEN=""`. The Kconfig option, the
validation and every use are gone. The only place the name still appears in the build is that
compile definition.

**Supported boards:** Muse boards with PSRAM only (`CONFIG_MUSE_HATCH`, which includes the AIPI).
Link-only and Voice PE builds depended on the deleted transport. `components/muse/CMakeLists.txt`
now stops them with a clear `FATAL_ERROR`. The SDK's own pytest suite under `esp32/tests/` covers
upstream Home Link code, and many of its tests target deleted files. It is not part of this
patch set's checks.

### The backend (`hatch/`)

- **`vesper_proto.{c,h}`** is the protocol core, pure C with no ESP-IDF dependencies. It covers:
  - request headers, with header-injection-safe token and node-id checks;
  - base URL parsing;
  - `audio_url` resolution, relative references only, so the bearer never leaves the
    configured server;
  - bounded SSE framing (comments skipped unbuffered, splits anywhere, CRLF/LF/CR);
  - flat-JSON field decoding (escapes, surrogate pairs);
  - UTF-8-safe truncation;
  - HTTP status verdicts.

  The firmware and the host tests compile the same file.
- **`muse_chat_vesper.c`** is the ESP-IDF backend behind the `muse_hatch_*` seam
  (`turn_begin/audio/end/cancel/event/read/caption`, plus status, test and the MP3 self-test). It
  uses `esp_http_client`:
  - It opens a chunked POST at the press and streams the note while the button is held.
  - It parses the SSE reply on the hatch task.
  - Text deltas become captions.
  - `message_done.audio_url` goes to `vesper_tts_slot_offer()`, **the task-10 TTS slot**. Task 09
    ships only a weak default that declines, so replies stay captions paced over silence, as in
    stock firmware.

  Logs carry lengths, statuses and timings only. They never carry the token, transcript or reply
  text.
- **Wire protocol:** `docs/node-wire-protocol.md` v1. Task 09 added the *Node firmware notes*
  section; the wire shape is unchanged.

### Configuration and provisioning a dev board

NVS namespace `muse`:

- **`host`** is repurposed as the server **base URL**, at most 63 characters. Turns go to
  `<host>/turn`.
- **`node_token`** is a new key for the node bearer. The stock `token` key may hold a Meta device
  token and is never read.

Both are empty until provisioned. The Kconfig fallbacks `CONFIG_VESPER_NODE_URL` and
`CONFIG_VESPER_NODE_TOKEN` default to `""`. Never commit a token in them. NVS is still plaintext;
NVS encryption is task 11's call.

With BLE pairing (`ble_server.c`) gone, the build has **no BLE host**. The claim flow (task 11)
brings BLE setup back. Wi-Fi and the backend are provisioned over **Muse's USB serial console**.
Every line starts with `>`. `wifi.pass` and `hatch.token` are never echoed.

```
>wifi.ssid=<network>
>wifi.pass=<password>
>wifi.connect
>hatch.host=https://peggy.fly.dev/vesper-node      (or a LAN URL, see the human gate)
>hatch.token=<VESPER_NODE_TOKEN>                    (from ~/.config/vesper-voice/node.env)
>hatch.test                                         (GET <host>/healthz with the bearer)
>status                                             (JSON; "hatch":{"token":true,...})
```

You can also join Wi-Fi from Muse's on-screen Wi-Fi settings, or with the
`CONFIG_HOMEHUB_WIFI_SSID` dev override. A setup reset from the Muse menu forgets the saved Wi-Fi
and the node token and keeps the server URL.

### Host checks (no board)

- `make -C firmware test` runs the protocol core's unit tests under ASan and UBSan, with
  `-Wall -Wextra -Werror`. They cover:
  - header building;
  - SSE framing at every split size, with the 2 KB comment preamble;
  - CRLF/CR line endings;
  - concatenating multiple deltas;
  - error, done and cut streams;
  - `audio_url` resolution and refusal;
  - oversized lines and events;
  - UTF-8 truncation;
  - JSON escapes;
  - HTTP verdicts.
- `make -C firmware live-turn` runs one real turn against the running node backend
  (`http://[::1]:8796` by default, `VESPER_NODE_URL` to override). It uses the same
  `vesper_proto.c` and libcurl in place of `esp_http_client`:
  1. `say` and `afconvert` make a 16 kHz mono PCM16 note.
  2. The note is sent as the firmware's streaming WAV, chunked.
  3. The SSE reply is parsed in 37-byte pieces.
  4. The resolved MP3 URL is fetched with the bearer.

  The token is read from `~/.config/vesper-voice/node.env` at runtime and never printed.

### Simulator

The simulator (`esp32/simulator/CMakeLists.txt`) builds only the UI (`muse_ui`, `muse_state`,
`muse_text` and the default avatar). It compiles no transport, so the backend swap leaves it as it
was, and `ctest` stays green on the patched tree. It doesn't exercise the Vesper backend. The host
tests above do that.

### On-device check (task 09 DoD, human gate)

The physical board is not flashed by the agents. Steps for Kevin:

1. **Get the backend reachable from the board.** Before task 12 (Peggy `/vesper-node/*`), the
   board can reach the Studio only over the LAN. The backend binds `::`, which is IPv6-only on
   the Studio, so either give the board an IPv6 route or add a temporary IPv4 forward on the
   Studio, for example:

   ```sh
   socat TCP4-LISTEN:8797,fork,reuseaddr TCP6:[::1]:8796
   ```

   Then use `http://<studio-LAN-IPv4>:8797` as the host. After task 12 the host is
   `https://peggy.fly.dev/vesper-node`.
2. **Build, flash and provision:**

   ```sh
   git -C ~/builds/muse-charm/scratch/muse-gadget-sdk worktree add --detach ~/builds/muse-charm/scratch/sdk-vesper b1a3822
   firmware/apply-sdk.sh ~/builds/muse-charm/scratch/sdk-vesper
   cd ~/builds/muse-charm/scratch/sdk-vesper/esp32
   tools/muse/board.sh build aipi && tools/muse/board.sh flash aipi /dev/cu.usbmodem83201
   . ~/esp/esp-idf-v6.0.1/export.sh && idf.py -B build-muse-aipi -p /dev/cu.usbmodem83201 monitor
   ```

   In the monitor, type the `>wifi.*` and `>hatch.*` lines above, then `>hatch.test`. Expect
   `server check: HTTP 200`.
3. **Run a turn.** Hold the talk button, say "what is two plus two", and release. Expect:
   - the screen to show `"what is two plus two"`, then the reply caption;
   - the serial log to show `turn: HTTP 200`, `transcript: N chars`,
     `message 0 done (... with speech)` and `turn done`;
   - the backend log (`~/Library/Logs/vesper-node/`) to show the turn with
     `X-Node-Id homelink-<mac>`.

   To roll back to stock, flash a build from the untouched SDK clone.

## Decision: the avatar is a patch set tracked in THIS repo (not an SDK fork)

`firmware/avatar/muse_pixel.c` is the source of truth. `firmware/avatar/install.sh [SDK_DIR]`
copies it (idempotently) to `<SDK>/esp32/components/muse/avatar/muse_pixel.c`.

Why:

- The SDK already has an explicit hook for this: `components/muse/CMakeLists.txt` globs
  `components/muse/avatar/muse_pixel.c` and uses it in place of the default avatar. That directory
  is ignored by the SDK repo, so installing the file touches no tracked SDK file and the SDK stays
  a pristine, re-clonable upstream checkout.
- The avatar is independent of transport. Task 09 (F1) later chose the same approach for the
  transport (see above), and `apply-sdk.sh` calls `install.sh`.
- The art is Vesper-owned; keeping it here keeps it out of any Meta-derived tree and reviewable
  next to the specs.

Caveat: the simulator (`esp32/simulator/CMakeLists.txt`) hard-codes the default avatar path
(`../../avatar/muse_pixel.c`) and does not use the custom one. The simulator `ctest` therefore does
not exercise Vesper's renderer; the avatar is covered by `make_gifs.py` (host render of every
mode) and by the ASan/UBSan bench run below. Making the simulator use it would mean editing a
tracked SDK file, which this task does not do.

## The avatar: Vesper, the evening-star owl

Original 64x64 palette-indexed art (31 palette entries, Bayer-dithered shading, 1 px outline).
A plump indigo owl with a pale moon face disc, ear tufts, big dark glinting eyes, small gold beak
and feet, and the evening star (a four-point sparkle) on its forehead. The star, aura, sparkles
and UI accent take the mode colour: gold at idle (a deliberate deviation from the SDK's violet
default), cyan listening, magenta thinking, mint speaking, red error, white/blue boot, dusk
violet off.

| Mode | Behaviour |
|---|---|
| BOOT | pops up from a squash, eyes open at 0.9 s, star ignites at 1.1 s, sparkles appear one by one |
| IDLE | slow bob, wings sway, random ear flicks, blinks (with double blinks), wandering gaze, pulsing star |
| LISTENING | ears perked, wide eyes, small open beak, raised brows, wings raised like cupped ears, expanding rings and sound arcs, star pulses with `level` |
| THINKING | eyes glance up and side to side, "hmm" beak, one wing folded to the chin, lean, one ear up, stepping thought dots, fast star flicker |
| SPEAKING | beak opens with `level` (plus flutter), body bobs with the voice, wings gesture, feet shuffle, rings and arcs, star flares with `level` |
| ERROR | X eyes, flat beak, 0.6 s shake, drooping ears, "!" mark, red scheme, star flickers (`happy` ignored) |
| OFF | right wing waves goodbye, eyes close, ears droop, glow fades over 1.3 s |
| happy overlay | hop, wings up and wiggling, `^^` eyes, open grin, ears up, big star, two hearts rising |

Previews: `firmware/avatar/gifs/*.gif` (192 px, one per mode plus `happy`), generated with
`python3 tools/muse/make_gifs.py` in the SDK and shrunk by `firmware/avatar/shrink_gifs.py`.

## Render time vs the 10 ms S3 budget

`firmware/avatar/bench.c` times `muse_pixel_render()` plus a full 128x128 `muse_pixel_scale()` per
frame, for every mode with and without the happy overlay (1950 frames each, `cc -O2`):

    cc -O2 -I <SDK>/esp32/components/muse firmware/avatar/bench.c firmware/avatar/muse_pixel.c -lm -o /tmp/vesper_bench && /tmp/vesper_bench

Host (Apple M4 Max, arm64, -O2): render mean 11-15 us (max seen 53 us), scale128 mean ~3 us;
worst single frame render+scale 48 us. The same code builds clean with
`-Wall -Wextra -Werror` and runs clean under AddressSanitizer + UBSan.

S3 estimate (240 MHz Xtensa LX7, 1 single-precision FPU, no sqrt/sin hardware): the M4 is roughly
150-250x faster per core on this kind of scalar integer code (about 4.5 GHz at ~6 IPC vs 240 MHz at
~0.7-1 IPC). That puts render at about 2-4 ms and a full scale at about 0.5-1 ms: roughly
3-5 ms per frame, comfortably under 10 ms even with a 2x safety margin for internal-RAM cache
misses. Per-pixel work is Q12 integer inside the body, wing, face and aura bounding boxes; floats
are used per frame or per part only: about 70 sqrtf (one per body row), a few dozen
sinf/cosf/expf, and the tiny eye/blush/foot ellipses (a few tens of pixels). On the S3's FPU these
are a few thousand cycles in total (under 0.3 ms), so there is no float/libm hot spot. No
allocation; static RAM is about 8 KiB of planes plus palettes and a 512-byte map. This is a host
proxy, not a measurement; confirm on hardware in task 05 if desired.
