# Wake word ("Hey Vesper") on the AiPi Lite: investigation

Status: **design investigation, no code changed.** Written 2026-10-08 against SDK `b1a3822` +
this repo's patch set (firmware 1.0.2). SDK paths are relative to
`~/builds/muse-charm/scratch/muse-gadget-sdk/esp32/`. Anything not measured on the board is
marked **UNVERIFIED** or **ESTIMATE**.

## TL;DR / recommendation

1. **microWakeWord, with a "Hey Vesper" model we train ourselves (recommended).** It is
   Apache-2.0 and free. The model is about 55-60 KB, embedded in the app image, so **OTA can ship
   it and the partition table doesn't change**. The tensor arena is about 23-26 KB. Training is
   self-serve from synthetic (Piper TTS) samples, so we control the word, and retraining is ours
   to do. The catch: we write our own ESP-IDF glue on `esp-tflite-micro`. ESPHome's C++ component
   is GPLv3, so we read it and don't copy it. Training is also documented as "early release,
   advanced users".
2. **ESP-SR WakeNet (9/10) with an Espressif-trained "Hey Vesper" (parallel track, free).** It is
   the most integrated option for ESP-IDF and has published accuracy figures (about 96% accept,
   about 1 false accept per 12 h). But we can't train it ourselves. A free model needs a request
   on `espressif/esp-sr#88`. "Hi Vesper" was requested there by someone else on 2026-02-22 and
   has **not been delivered** in about 7.5 months. Any delivered model is public. Models load
   from a separate flash partition, which means a **one-time USB reflash per node** (OTA can't
   change the partition table). Recommendation: post a "Hey Vesper" request now (costs nothing),
   and build on microWakeWord meanwhile.
3. **Server-side openWakeWord (Studio) with the node streaming idle audio: not recommended.**
   The node reaches the backend through Peggy (`https://peggy.fly.dev/vesper-node`). Streaming
   256 kbit/s of always-on audio through a public edge, around the clock, is wrong for privacy,
   power and cost.
4. **Picovoice Porcupine: not viable.** Free-tier AccessKeys stopped working on 2026-06-30, no
   non-commercial tier is planned, and its MCU builds target Arm Cortex-M/STM32, not ESP32-S3.
5. **Espressif paid customization: overkill.** It needs at least 20,000 utterances from more than
   500 speakers, a fee, and 2-3 weeks of training.

Plan shape: a **1-2 day spike** first, running a stock microWakeWord model ("hey_jarvis") on the
board. It answers the three open questions, which are CPU on core 1, false triggers in Kevin's
house, and battery drain. Then train "Hey Vesper", then the integration tasks. Total effort is
about **6-9 working days** (breakdown in §6).

Policy recommendation: **wake word on whenever the board is on USB power or awake on battery.
Off while asleep on battery**, unless an opt-in setting says otherwise (see §4 for why).

---

## 1. Engine options

| | microWakeWord | ESP-SR WakeNet9 / 9l / 10 | openWakeWord | Porcupine | Espressif paid custom |
|---|---|---|---|---|---|
| Runs on ESP32-S3 | Yes (TFLite Micro + esp-nn; ESPHome ships it on S3) | Yes (native, Espressif) | **No** (server/RPi class: melspec + ~1 M-param embedding net in ONNX/TFLite) | **No** port for ESP32 found (MCU = Cortex-M/STM32) | Yes (WakeNet) |
| License / cost | Apache-2.0 (trainer `kahrendt/microWakeWord` + `esphome/micro-wake-word-models`); free. ESPHome's *C++ runtime* is **GPLv3**, so don't copy it | "Espressif MIT": free, **only on Espressif chips**; community models "free for commercial use" | Apache-2.0 code; models CC-BY-NC-SA (not relevant here, personal use) | Free tier ended 2026-06-30, enterprise-only | Fee (unpublished) + corpus collection fee |
| Model size | 52-60 KB `.tflite` (alexa 55,856 B; hey_jarvis 52,272; okay_nabu 60,264); optional VAD model 34,328 B | wn9: ~290 KB (`wn9_data` 289,797 B); wn10: ~1.0 MB (two parts, 475,840 + 525,952 B) | ~3 MB total + classifier | n/a | wn9-sized |
| RAM | tensor arena 22,860 B (hey_jarvis) / 26,080 B (okay_nabu); VAD +16,772 B; plus feature frontend + task stack (**ESTIMATE** few KB) | wn9: **16 KB internal + 324 KB PSRAM**, 3.0 ms per 32 ms frame (Espressif S3 benchmark). wn10: **UNVERIFIED** (bigger model) | n/a on device | n/a | as WakeNet |
| CPU | Inference every 30 ms (10 ms feature step, stride 3); per-inference time on S3 **UNVERIFIED**, measure in the spike | ~9.4% of one 240 MHz core (3.0/32 ms) | n/a | n/a | as WakeNet |
| Accuracy | No published FAR/FRR; tuned per model by `probability_cutoff` (0.97 stock) + `sliding_window_size` (5). Quality depends on our training | wn9 Alexa (human-trained): 94-98% at 3 m, ~1 false wake per 12 h. TTS-trained community models: RAR 94-97% at FAR 1 per 12 h (Espressif's figures per delivered model) | good on server | good | best |
| "Vesper" trainable? | **Yes, by us**, from synthetic samples (Piper sample generator + Hugging Face negative sets + `basic_training_notebook.ipynb`) | Only via Espressif: free TTS-trained model on request (#88, English supported, queue-based, needs a project link or 5 upvotes), or the paid service. "Hi Vesper" requested 2026-02-22, not delivered | Yes, easy (Colab notebook), but server-only | n/a | Yes |
| Ship path | Model compiled into the app image → **OTA-able** | Model in a `model` data partition (`srmodels.bin`) → **partition-table change = USB reflash per node** (current table has no model partition; ~7.8 MB of the 16 MB flash is unallocated past `0x822000`, so it fits) | streaming | n/a | as WakeNet |
| ESP-IDF v6.0.1 compat | `esp-tflite-micro` 1.4.1 requires IDF ≥5.1 (v6 **UNVERIFIED**) | `esp-sr` 2.5.5 requires IDF ≥5.0 + `esp-dl ~3.3.12`; v6 **UNVERIFIED**, and one #88 user reported reverting from IDF 6.1 to 5.3.6 after hardware testing (reason not stated) | n/a | n/a | as WakeNet |

Notes on "Vesper" as a word. "Hey Vesper" is about 3-4 syllables (/heɪ ˈvɛs.pɚ/). Longer, rarer
phrases reject false triggers better than 2-syllable ones, which is in our favour. Espressif asks
for 3-6 "symbols" (syllables). The risk words are near-homophones that a TTS-trained model may
not have seen as negatives: *whisper*, *vesper(s)* in speech or TV, *Vespa*, *best for*. Add
them as explicit negatives when training microWakeWord. The bare word "Vesper" alone (2
syllables) would trigger noticeably more often. Keep the "Hey".

Not considered further: ESP-SR MultiNet (command recognition, not a wake word); Edge Impulse KWS
(possible, but it's the same TFLM path as microWakeWord with a worse training story for
synthetic data and a proprietary studio); DIY MFCC + DTW template matching (poor accuracy).

## 2. Current mic pipeline (traced from source)

**Hardware.** One ES8311 codec does both mic ADC and speaker DAC, over **one duplex standard
(Philips) I2S bus**, `I2S_NUM_0`, ESP32 is master. Control is over I2C0 (SDA 5 / SCL 4, ES8311
default address). The pins are MCLK 6, BCLK 14, WS 12, DIN 13 (mic → S3) and DOUT 11, with the PA
enable on GPIO9. The codec is clocked from BCLK (`use_mclk=false`, as in xiaozhi). This is **I2S,
not PDM**. There is **one mic, on the left slot** (`mic_slot = 0`).
(`components/muse/boards/board_aipi.c:audio_init`, via `espressif/esp_codec_dev ~1.5`.)

**Format.** 16 kHz, 16-bit, 2 slots (stereo frame, mic on L)
(`muse_audio.h`: `MUSE_AUDIO_RATE 16000`, `MUSE_AUDIO_CHUNK 320` = 20 ms). The I2S channel uses
the IDF default DMA config (`I2S_CHANNEL_DEFAULT_CONFIG`, `auto_clear`); the exact DMA
descriptor count/frame count in IDF 6.0.1 is **UNVERIFIED**, but by default it is on the order of
~100 ms of buffering. So a reader stall much longer than that drops samples.

**Read path** (`components/muse/muse_audio.c:muse_audio_read`):
`esp_codec_dev_read(s_mic, s_in_stereo, 320 frames × 2 ch × 2 B = 1280 B)`. The left sample is
kept, then a **one-pole 80 Hz high-pass** (DC/rumble), clamped to int16 mono. Mic gain is the
ES8311 PGA, from the `Mic gain` menu (`muse_settings_mic_gain`).

**Who reads it.** One task only: `muse_voice` (`components/muse/muse_voice.c`), pinned to
**core 1** (`MUSE_AUDIO_CORE`), priority 6, 6 KB stack in PSRAM.

- **Idle** (`voice_task` → `idle_capture()`). A blocking 20 ms read paces the loop. Each chunk
  feeds the settings mic meter and a **pre-roll ring of 16 × 20 ms = 320 ms** (PCM in PSRAM).
  The first `SETTLE_CHUNKS` = 10 (200 ms) after any playback are skipped so Muse doesn't hear
  itself. **The mic is already always on whenever the board isn't "resting".** This is the
  natural tap point for a wake-word detector.
- **PTT press** → `record()`. Pre-roll first, then 20 ms chunks into `take()`, which streams to
  `muse_hatch_turn_audio()` (our `firmware/hatch/muse_chat_vesper.c`: a chunked POST `/turn`,
  WAV header + PCM16, on the `muse_chat` task, core 0, 32 KB PSRAM stack). It keeps a whole copy
  in a 15 s PSRAM buffer (`s_rec`) for offline "held notes". On release it records 120 ms more,
  then `finish_note()` → falling chirp → `hatch_reply()`, which plays MP3→PCM from
  `muse_hatch_turn_read()`. A PTT DOWN during the reply is a barge-in (it skips 200 ms of
  capture: the reply's tail).
- **Resting** (`set_resting`): only when *asleep and on battery*. `muse_audio_power(false)`
  closes both codecs, so ADC, DAC, PA and I2S stop and the mic is off.

**Events.** Buttons (and the serial `d`/`u` keys) become `muse_input_event_t {MUSE_PTT_DOWN|UP,
wake}` posted by the static `post()` in `muse_input.c` onto the queue `muse_voice` reads.
There's no exported way for other code to post a PTT event today.

## 3. Compute / memory budget

**Board.** ESP32-S3R8, so **8 MB octal PSRAM** (80 MHz, XIP/rodata in PSRAM), **16 MB flash**,
dual-core 240 MHz (`devices/sdkconfig.muse-aipi`, `sdkconfig.defaults`). 512 KB SRAM on-die,
which after IRAM/caches (32 KB I + 64 KB D) leaves the internal heap that matters.

**Measured idle** (Vesper firmware, serial heartbeat in `/tmp/ptt-run.log`, 2026-10-08, Wi-Fi
connected): `int=119K/62K` (free / largest block), `dma=111K/62K`, `psram=5446K`. For comparison
the stock Meta build had `int=31K` (Oct 7 log). Deleting the Noise transport freed most of that.
PSRAM was 5,223 KiB free in the task 10 heartbeat. **Caveats:** I couldn't tell from the log
whether the BLE host was running; it costs internal RAM when it starts for a claim. During a turn
TLS + Wi-Fi TX take internal RAM (the reason `CONFIG_MBEDTLS_HARDWARE_AES=n`). The worst-case
internal low-water during a turn + TTS is **UNVERIFIED**. `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`
= 64 KB stays held back for DMA/internal-only allocations.

**What a wake engine adds:**

| Item | microWakeWord | WakeNet9 |
|---|---|---|
| Internal RAM | arena ~23-26 KB if internal (it can go in PSRAM, slower) + ~4-6 KB task stack (or PSRAM stack; this task never writes flash) + frontend state (few KB) | 16 KB |
| PSRAM | model ~60 KB (rodata → PSRAM via `SPIRAM_RODATA`), audio ring ~8 KB | 324 KB |
| Flash | ~60-100 KB in the app image (app partition is 51% free of 4 MB) | ~290 KB (wn9) / ~1 MB (wn10) in a new `model` partition + esp-sr/esp-dl code in the app |
| CPU | **UNVERIFIED**, measure (expected a few % of a core) | ~9.4% of one core |

Both fit comfortably in RAM: about 30 KB of 119 KB internal free, and well under 5.4 MB PSRAM.
**CPU placement is the real question.** Core 1 already runs `muse_voice` (prio 6) and LVGL (prio
5, 40 ms frames; the avatar renders in about 1 ms/frame, estimated from the host). Core 0 runs
Wi-Fi/lwIP, `muse_chat` (HTTP/SSE, MP3 decode) and the announcement poll. Proposal: a
`vesper_wake` task on **core 1 at priority 4** (below the UI). `muse_voice` stays the only mic
reader. `idle_capture` copies each 20 ms chunk into a FreeRTOS stream buffer (non-blocking, drop
on full), so inference can never delay the 20 ms read and overrun the I2S DMA. The spike checks
that avatar frame times and `muse_voice` cadence don't move.

## 4. Power

**What the firmware does today** (`muse_input.c`, `muse_voice.c`, `sdkconfig.muse-aipi`):

| State | Mic/codec | CPU | Wi-Fi | Screen |
|---|---|---|---|---|
| USB power (any) | **on, always** (idle pre-roll) | 240 MHz, no light sleep | full | per auto-sleep |
| Battery, awake (≤ `sleep_s`, default **120 s** after last activity) | **on** | 240 MHz | doze (modem sleep between beacons) | on |
| Battery, asleep | **off** (codecs closed) | crystal clock + **auto light sleep** between button waits (`CONFIG_PM_ENABLE`, tickless idle) | rest; **off ("nap") after 2 min** | off, LVGL paused |

So **on USB the wake word costs essentially no extra power**: the mic and CPU are already
running. On battery while awake it adds only inference CPU. The whole cost is in the **battery,
asleep** state, which is the board's real low-power mode and is worth keeping.

**ESTIMATE** (orders of magnitude from ESP32-S3 / ES8311 datasheet-class figures, *not measured
on this board*, 450 mAh cell):

- Asleep today: the CPU light-sleeps, the radio is off after 2 min and the codec is off. That's
  roughly ~1-5 mA averaged (the board's regulator, the charger and the LCD controller in SLPIN
  set the floor; **UNVERIFIED**), so days of standby.
- Asleep, listening: always-on would need I2S DMA + codec ADC running, 30 ms inference ticks
  (that blocks light sleep: the I2S driver holds a PM lock), and Wi-Fi at least dozing so a turn
  doesn't wait for a several-second rejoin. That's roughly ~30-60 mA, so on the order of **8-15
  hours** on the 450 mAh module. That is a different product (a desk puck on USB) from a
  wearable charm.

**Measure, don't guess.** The firmware already has a battery meter. `>power` on the console gives
time actually slept and the per-PM-lock awake time (`CONFIG_PM_PROFILING`), and
`muse_battery_drain()` gives %/h after 10 min. The spike should record drain in three states:
asleep (baseline), awake idle without wake word, and awake idle with wake word.

**Recommendation:** wake word **active on USB power and while awake on battery**; **inactive
while resting** by default. An opt-in `Listen while asleep` setting would keep codecs on (no
`set_resting(true)`) but still let the display pause. It would stop the light sleep, so the
setting text should say so ("~10 h battery").

## 5. Coexistence with push-to-talk (UX design)

**Both are always active. PTT always wins.** The wake word is one more way to start the same
`record()` → `finish_note()` → `hatch_reply()` turn. The engine runs only in IDLE.

- **When the wake word listens:** mode IDLE, codecs on, menu closed, no claim code being shown
  for the first time (optional), and not within 200 ms after any playback (the existing
  `SETTLE_CHUNKS`). It does **not** listen during LISTENING/THINKING/SPEAKING or announcement
  playback. There's no AEC: the single mic has no playback reference, and ESP-SR's AFE AEC needs
  one. On the AIPI the ES8311's ADC right slot as a reference is **UNVERIFIED**. So the reply
  must not be able to trigger it. Voice barge-in ("Hey Vesper, stop") is **out of scope for
  v1**. The button barge-in stays.
- **On trigger:** start the turn exactly as a PTT press does, but:
  - **No trailing button release**, so the turn needs an **end-of-speech detector**: energy VAD on
    the 20 ms chunks (microWakeWord's 34 KB VAD model, or the dBFS already computed for the
    meter), with ~800 ms trailing silence, ≥ 300 ms speech required, and the 15 s `MAX_SECS` cap
    kept.
  - **No-speech timeout:** if nothing is said within ~4 s of the trigger, cancel quietly
    (`muse_hatch_turn_cancel`, caption `DIDN'T CATCH THAT`, back to IDLE). Nothing is uploaded if
    the turn hadn't gone live; see below.
  - **Pre-roll:** skip the 320 ms pre-roll for wake turns. It only holds the wake phrase. Start
    from the trigger instant.
- **Feedback:** the avatar goes straight to **LISTENING (cyan)** with caption `LISTENING...`, as
  for PTT. `record()` deliberately plays no start chirp (it would land on the first words). For a
  wake word, a short soft "ding" is the convention users wait for. Recommendation: **visual only
  by default**, with an optional `Wake chirp` setting. If the chirp is on, skip the 200 ms settle
  after it. The end-of-turn falling chirp stays as it is. Waking from screen-dim works as a talk
  press does (`muse_state_poke`).
- **PTT during a wake turn:** a press during a wake-triggered LISTENING turns it into a PTT note
  (the release ends it). That's the "I'll hold it myself" escape. A press during THINKING or
  SPEAKING is the existing barge-in.
- **False triggers:**
  - Silence after a false trigger costs nothing (the no-speech cancel).
  - Overheard speech becomes a real turn. The backend gets a transcript and may answer, which is
    the visible failure. Mitigations: a high cutoff + sliding window, explicit near-homophone
    negatives in training, and **upload only after the VAD confirms ≥ 300 ms of speech after the
    trigger**. Until then the audio stays in the existing `s_rec` buffer and `go_live()` is
    deferred, so a pure false trigger never leaves the device.
  - **Counters** for tuning: `>status` gains a `wake` block (`armed`, `triggers`,
    `cancelled_no_speech`, `turns`) and the log line carries the score. **No audio and no
    transcript in logs** (log hygiene, as `check_log_hygiene.py` enforces).
- **Off switch:** `wake.enabled` (NVS `muse:wake_on`, default **on**), settable over the serial
  console / BLE (`>wake=on|off`), the same way `hatch.*` is. Putting it in the on-screen menu
  means patching `muse_menu.c`, a kept-identical UI file, so that's **Kevin's decision**. A small
  "ear" glyph or a caption on the idle screen when armed would be nice but touches `muse_ui.c`
  (also kept-identical); defer that.
- **Offline:** if `muse_hatch_ready()` is false, a wake turn records into a held note as PTT does
  today. Or, simpler and arguably better, it doesn't arm while offline. Recommendation: **don't
  arm offline.** A held note nobody intended to record is surprising.
- **Privacy:** identical to today. The mic is already sampled into a RAM ring at idle (pre-roll).
  The wake engine sees the same chunks, and nothing leaves the device until a turn goes live.

## 6. Firmware + backend changes and effort

### Constraint this runs into

`firmware/README.md` lists `muse_voice.c` (and `muse_input.c`'s neighbours: the UI and menu) as
**kept byte-identical**. Only `main/voice.c` is *enforced* by `apply-sdk.sh`, but the README
promises the rest. Wake word can't be done without touching `muse_voice.c`: it's the only mic
reader, and turns are driven from its loop. **Decision needed:** accept a small `0007` patch to
`muse_voice.c` + `muse_input.c` and update the README's kept-identical list. Keep the patch to
**hooks only**, in the established weak-default seam style (like `vesper_tts_slot_offer`):

- `idle_capture()`: call `muse_hatch_idle_audio(s_chunk, n)` (weak no-op default).
- `record()`: per-chunk `muse_hatch_turn_vad(pcm, n)` returning "end now" for wake turns, a
  start flag (wake vs press) that skips the pre-roll, and deferred `go_live()`.
- `set_resting()`: honour `muse_hatch_keep_listening()` (weak, false).
- `muse_input.c`: export `muse_input_post_ptt(type, wake)` (it wraps the static `post()`), so the
  wake task injects a DOWN, and VAD/timeout ends the turn through the same event path.

All the logic (engine, VAD, policy, counters) lives in a new `firmware/hatch/vesper_wake.{c,h}`,
with a pure-C, host-testable core like `vesper_proto.c` / `vesper_announce.c`.

### Tasks

| # | Task | Notes | Effort |
|---|---|---|---|
| 0 | **Post a "Hey Vesper" request on `esp-sr#88`** (link the repo, upvote the existing "Hi Vesper") | free fallback track; no dependency | 15 min |
| 1 | **Spike** (throwaway branch): add `esp-tflite-micro` to the AIPI build on IDF 6.0.1, run stock `hey_jarvis.tflite` off `idle_capture` chunks, log score / inference µs / heap. Measure: core-1 load, avatar frame time, voice-loop cadence, internal low-water across a full turn + TTS, battery drain (`>power`) awake with and without it; count false triggers over an evening of normal house noise (TV) | answers every UNVERIFIED that matters; go/no-go on microWakeWord and IDF 6 | 1-2 d |
| 2 | **Train "Hey Vesper"** with microWakeWord: Piper synthetic positives (many voices, speeds; "hey vesper" / "hi vesper"), negatives incl. *whisper, vespers, Vespa, best for, hey* + HF negative sets; pick cutoff/window from the false-accept-per-hour curve. Run on Colab or a CUDA box (Mac/TF-Metal for this notebook is **UNVERIFIED**). Validate with Kevin's (and family's) real recordings captured from the board via the serial console | iterative; quality lives here | 1-2 d |
| 3 | **`vesper_wake` core** (pure C, host-tested): feature frontend wrapper, streaming model invoke, sliding-window decision, VAD/endpoint state machine (no-speech timeout, trailing silence, min speech, max length), arm/disarm policy (mode, power, settle, offline, menu), counters | host tests in `hatch/test/` like the announcement scheduler | 1.5 d |
| 4 | **SDK patch `0007`**: the hooks above in `muse_voice.c` / `muse_input.c`; `vesper_wake.c` + `esp-tflite-micro` in `components/muse/CMakeLists.txt` + `idf_component.yml`; README kept-identical list updated | keep the diff small; `apply-sdk.sh` prefix logic already handles a growing series | 1 d |
| 5 | **Settings + console**: `>wake=on|off`, `>wake.asleep=on|off` (opt-in listen while resting), `>wake.chirp`; NVS keys; `>status` `wake` block; log-hygiene check covers the new file | | 0.5 d |
| 6 | **Backend (small)**: see below | | 0.5 d |
| 7 | **On-device DoD**: bump `hatch/VERSION` (1.1.0), build, OTA to the board (no partition change), 20 wake turns + 20 PTT turns interleaved, barge-in, false-trigger soak overnight (counter), battery drain in the three states, heap low-water; record in the task file | human gate for the soak and voice samples | 1 d |
| | **Total** | | **~6-9 d** |

If microWakeWord fails the spike (CPU or IDF 6 trouble), the fallback is to swap task 2 for an
ESP-SR model and add a partition-table task ("add a `model` partition after `0x822000`; USB
reflash every node once; `srmodels.bin` built from `esp-sr`"). Tasks 3-7 stay the same, since
the engine sits behind `vesper_wake`'s interface.

### Backend (`backend/`, wire protocol stays v1)

- **Optional request header `X-Turn-Trigger: wake|button`.** It's additive. I couldn't confirm
  from `docs/node-wire-protocol.md` that unknown headers are ignored (the doc lists the headers it
  validates; it doesn't state a policy for others), so **verify in `routes`** before relying on
  it. It is used for metrics and for the next two items.
- **Strip the wake phrase** from the start of the transcript before `/ask`: "hey/hi/ok vesper" plus
  STT mishearings (*whisper, vespa, vesper's*). If anything of the phrase got into the audio, the
  brain shouldn't see it.
- **Empty after stripping** → the existing `empty_transcript` error. Firmware maps that to a
  **silent** return to IDLE for wake turns (no `ERROR` face) instead of the PTT caption.
- No new endpoints, no auth change, no TTS change.

## Open questions for Kevin

1. OK to patch `muse_voice.c` / `muse_input.c` (hooks only), and drop them from the README's
   "kept byte-identical" list?
2. Default for battery-asleep listening: **off** (recommended) or on?
3. Start chirp: visual-only (recommended) or a soft "ding"?
4. Menu toggle for the wake word (patches `muse_menu.c`) or console/BLE only for v1?
5. Phrase: "Hey Vesper" (recommended) vs "Hi Vesper" vs "Vesper" alone. Training can include
   "hi" as an accepted variant.

## Sources

- Espressif ESP-SR WakeNet docs and S3 benchmark:
  <https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/wake_word_engine/README.html>,
  <https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/benchmark/README.html>
- Espressif customization process (corpus/fee/turnaround):
  <https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/wake_word_engine/ESP_Wake_Words_Customization.html>
- ESP-SR repo (model list incl. wn10 released 2026-08-17, license, `idf_component.yml` 2.5.5) and
  TTS wake-word request thread: <https://github.com/espressif/esp-sr>,
  <https://github.com/espressif/esp-sr/issues/88> (the "Hi Vesper" request is dated 2026-02-22)
- microWakeWord (Apache-2.0): <https://github.com/kahrendt/microWakeWord>; models + manifests
  (sizes, `tensor_arena_size`, cutoffs): <https://github.com/esphome/micro-wake-word-models>;
  ESPHome component: <https://esphome.io/components/micro_wake_word/>; ESPHome license (C++
  GPLv3): <https://github.com/esphome/esphome/blob/dev/LICENSE>
- esp-tflite-micro (Apache-2.0, 1.4.1): <https://github.com/espressif/esp-tflite-micro>
- Porcupine free-tier shutdown:
  <https://community.home-assistant.io/t/fyi-picovoice-confirmed-free-tier-accesskeys-will-stop-working-after-june-30-2026/1012744>;
  platform list: <https://github.com/Picovoice/porcupine>
- Local source: `components/muse/boards/board_aipi.c`, `muse_audio.{c,h}`, `muse_voice.c`,
  `muse_input.c`, `muse_mem.h`, `devices/sdkconfig.muse{,-aipi}`, `partitions_muse.csv`;
  `firmware/hatch/muse_chat_vesper.c`; `firmware/README.md`; heartbeat logs `/tmp/ptt-run.log`,
  `/tmp/vesper_ptt/turn.log`.
