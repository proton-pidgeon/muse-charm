# firmware/

Vesper-specific firmware pieces on top of Meta's Muse gadget SDK at commit `b1a3822`. A
pristine clone lives at `~/builds/muse-charm/scratch/muse-gadget-sdk`. Every change to the SDK is
tracked here, and `firmware/apply-sdk.sh` turns a pristine checkout into the Vesper node firmware.

| Path | What |
|---|---|
| `apply-sdk.sh <SDK_DIR>` | Idempotent. Deletes the Meta transport, applies `sdk-patches/*.patch`, installs `hatch/` and the avatar, and checks that `main/voice.c` is unchanged |
| `sdk-patches/delete.txt` | Files the patch set removes from the SDK |
| `sdk-patches/000N-*.patch` | Edits to tracked SDK files (`git apply` format against `b1a3822`) |
| `hatch/` | The Vesper `muse_hatch_*` backend (task 09) and its reply speech (task 10), installed as `<SDK>/esp32/components/muse/vesper/` |
| `hatch/test/` | Host tests of the protocol core and the speech helpers, and a live-turn harness |
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
   - the backend selection at `components/muse/CMakeLists.txt` (since task 10 it also builds
     `vesper/vesper_audio.c`: one line, the only SDK-side change task 10 needed);
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
- **`vesper_audio.{c,h}`** (task 10) is the reply-speech arithmetic, pure C as well: the
  22.05 kHz -> 16 kHz resampler, the MP3 buffer accounting of the decode path, and the caption
  timing numbers. See *Speech* below.
- **`muse_chat_vesper.c`** is the ESP-IDF backend behind the `muse_hatch_*` seam
  (`turn_begin/audio/end/cancel/event/read/caption`, plus status, test and the MP3 self-test). It
  uses `esp_http_client`:
  - It opens a chunked POST at the press and streams the note while the button is held.
  - It parses the SSE reply on the hatch task.
  - Text deltas become captions.
  - `message_done.audio_url` goes to `vesper_tts_slot_offer()`, **the TTS slot**, which task 10
    filled: the reply is spoken (see *Speech* below). Task 09 shipped a weak default that declined.

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

### Speech (task 10, F2)

Stock firmware never spoke gadget replies: `start_tts()` in `muse_chat_session.cpp:1503-1517` was
an empty slot that paced captions over silence (conflict C4). The Vesper backend fills it.

- **Where.** `on_message_done` hands `audio_url` (already resolved by `vp_resolve_audio_url`, so on
  the configured server) to `vesper_tts_slot_offer()`, which only records it: no I/O inside the
  SSE parser. `start_showing()` (the stock `start_tts()`) takes the finished messages in order. A
  message with a URL is fetched: `GET` with the turn's `Authorization` and `X-Node-Id`,
  `Accept: audio/mpeg`, no redirects, 5 s connect/headers timeout. It needs `200` and
  `audio/mpeg`, and at most 2 MiB. Anything else falls back to silent pacing. The URL is never
  logged, because its id is a capability.
- **Decode.** This is the stock `tts_data`/`decode` path, on the hatch task (32 KB PSRAM stack;
  minimp3 needs ~16 KB). The body is read in 20 ms polls into a 192 KiB PSRAM buffer. Reading is
  flow-controlled: it reads only while the buffer has room, so a long reply never drops bytes.
  minimp3 decodes while the 2 s reply buffer (`s_out`) has room for a frame's output. Until the
  download ends it holds back the last 1,445 bytes, because minimp3 drops a frame it can't see
  past. Stereo is downmixed.
- **Resampling.** The backend sends `mp3_22050_32`, and the speaker runs at 16 kHz. Stock code
  resampled with linear interpolation. Going down from 22.05 kHz, that folds 8-11 kHz back into
  the speech: a 10 kHz tone comes out at 6 kHz only 4.6 dB down (host test). `vesper_audio.c`
  uses a polyphase windowed-sinc filter instead:
  - 48 taps, Blackman window, 128 phases, cutoff at 0.45 of the lower rate;
  - exact rational time base, so it never drifts;
  - unity DC gain in every phase;
  - its 24-sample lag is flushed at the end of each message.

  Measured: 1 kHz passes at gain 1.0000 with 63 dB SNR, and the 10 kHz alias is at -78 dB. The
  table takes 25 ms to build on the S3 the first time, and is kept for later messages at the same
  rate. The bench MP3 self-test uses the same resampler.
- **Playback.** Decoded PCM goes into the same `s_out` stream buffer that `muse_voice.c`
  (unchanged) reads with `muse_hatch_turn_read()` and writes to the 16 kHz speaker at the
  configured volume. `voice.c` and `voice_player.c` are untouched. The AIPI doesn't compile
  `voice.c`.
- **Caption sync.** `muse_hatch_turn_caption(played)` pages the message by `played - pcm_start`
  over `pcm_frames`, which comes from the audio:
  - while decoding: the samples out so far, plus the bytes left at the measured bytes per sample.
    It doesn't use the frame header's bitrate: the backend's first frame claims 56 kbps in a file
    that averages 32;
  - once drained: exact.

  Captions are no longer paced over silence when there is speech. One log line per page turn
  (`caption: message M page P at X s of Y s (char C of N)`) is the sync evidence. In sync,
  `C/N` follows `X/Y`.
- **Failures never wedge the turn.** In every case the turn still ends:
  - no URL, a refused URL, connect/HTTP/type failure, or no decodable frame: the stock pacing
    (16 chars/s plus a 2 s hold);
  - download cut short or stalled (10 s without bytes): what arrived is spoken, then the rest of
    the caption is paced over silence at the speech's own rate;
  - the 180 s turn cap still applies.
- **Cancel.** A new press (`turn_cancel`) bumps the generation and drains `s_out`. Then
  `CMD_CANCEL`, a failure, a settings change or the time cap reaches `turn_finish()`, which calls
  `tts_stop()` and closes the GET. Every write to `s_out` checks the generation first, so a
  cancelled turn's audio can't reach the speaker after the drain. The one blocking step is the
  GET's connect plus headers (5 s at worst; ~25 ms on the LAN). A cancel during it is handled
  right after, and the mic backlog (8 s) covers a press made meanwhile.
- **Memory.** The MP3 buffer (192 KiB), one decoded frame and its 16 kHz output are allocated
  once at start, all in PSRAM. The decoder and resampler state (~20 KB) are PSRAM `.bss`. No turn
  path allocates, so no error path has anything to free. If the start-up allocation fails,
  replies are captions only.

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

  The same target runs the speech helpers' tests (`hatch/test/test_vesper_audio.c`):
  - resampler length and drift: 3 s in gives 3 s out, split anywhere or fed per MP3 frame;
  - 1, 3 and 5 kHz pass on time and at level;
  - 9 and 10 kHz are filtered, compared against the stock linear interpolator;
  - DC exact, saturation, and 8/24/44.1 kHz input;
  - the coefficient table (unity gain, accumulator headroom);
  - MP3 buffer hold, flow control and drain, with a stand-in decoder that, like minimp3, needs
    the next header;
  - the caption-length estimate and the TTS Content-Type check.
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

### On-device check (task 10: spoken replies)

Done by the task-10 agent on 2026-10-08, with the coordinator's authorisation, on the AiPi at
`/dev/cu.usbmodem83201` (Wi-Fi already provisioned). Build from `sdk-impl-10`, flashed with
`board.sh flash aipi`. `hatch.host=http://192.168.5.16:8797` (the socat stopgap) and
`hatch.token` were set over serial, piped from `node.env` and never printed. `>hatch.test` gave
`server check: HTTP 200`. The question was "what is two plus two", played from the Studio's
speakers into the board's mic. The full logs are in `/tmp` (not committed). The serial log of
the first turn:

```
vesper_chat: voice note: 4.04s, 129324 bytes
vesper_chat: turn: HTTP 200
vesper_chat: transcript: 21 chars
vesper_chat: message 0 done (21 chars, with speech)
vesper_chat: tts: message 0: GET HTTP 200, Content-Length 5347, audio/mpeg, headers in 23 ms
vesper_chat: tts: fetched 5347 of 5347 bytes in 26 ms
vesper_chat: tts: reply audio 22050 Hz, 1 ch, 56 kbps -> 16000 Hz (resampler ready in 26 ms)
muse_voice: reply audio after 4.21s
vesper_chat: tts: message 0: 50 MP3 frames at 22050 Hz -> 20898 samples at 16000 Hz (1.31 s of speech); first audio 57 ms after the GET
vesper_chat: turn done: 1 message(s) in 8.1s
vesper_chat: caption: message 0 page 2 at 0.95 s of 1.31 s (char 15 of 21)
muse_voice: muse reply: 1.31s of audio, 5.52s total
```

- The speech ran 1.31 s, and 20,898 samples at 16 kHz is 1.306 s. That matches 50 MPEG-2
  layer III frames x 576 samples at 22,050 Hz, which is 1.306 s, so the resampling keeps the
  duration.
- `muse_voice` played 1.31 s of it to the speaker.
- The caption turned its page at 0.95 of 1.31 s (72%) at character 15 of 21 (71%).

A second turn (the reply "Four.": 2,944 bytes, 27 frames, 11,285 samples, 0.71 s played) was
just as clean. The PSRAM heartbeat was back at its idle level afterwards (5,223 KiB free), so
nothing leaks.

**Barge-in.** A press 0.2 s into a 1.31 s reply logged `reply interrupted`, then
`turn cancelled`. Nothing more of the old reply played. The 0.14 s press was dropped as too
short, so it never reached `/ask`.

**HUMAN GATE (Kevin): hearing it.** The serial evidence shows the PCM going to the speaker. Only
a person can confirm the sound. With the board flashed and provisioned as above (it still is):

1. Make sure the backend (`com.vesper.node`) and the socat forward on `:8797` are running.
2. Hold the talk button, say "what is two plus two", and release.
3. Within about 5-8 s you should hear Vesper's voice say the answer from the AiPi's speaker,
   with the caption paging along with it.
4. If it's silent but the log shows `muse reply: X s of audio` with X > 0, check the volume in
   Muse's settings (the speaker plays at `muse_settings_volume()`).

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
