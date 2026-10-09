# "Hey Vesper" wake word: quality bars and evaluation protocol (task 21)

Status: **fixed before any training run.** Written 2026-10-08, before the first model was
trained. These bars and this protocol don't change after we see results. If a model misses
them, it fails. We don't move the bar.

## The bars

| # | Metric | Bar | Measured on |
|---|---|---|---|
| B1 | False-reject rate (FRR) | **< 5 %** | held-out synthetic "hey vesper" test clips, noisy/reverberant condition (below) |
| B2 | False accepts per hour (FA/h) | **< 1.0 per hour** | held-out background audio (below), total over all sources |
| B3 | Model size | **≤ 65,536 bytes** (64 KiB) | the committed `hey-vesper.tflite` file, byte for byte |

B1 and B2 are both measured at **one** operating point: the `probability_cutoff` and
`sliding_window_size` the firmware will ship. That point is chosen on the **validation** split
only (rule below). It is never tuned on the test split.

## What gets evaluated

- **The final artifact only.** The int8-quantized, streaming, internal-state `.tflite`
  (`stream_state_internal_quant.tflite`, committed as `firmware/wakeword/hey-vesper.tflite`).
  It runs through the TFLite interpreter exactly as the firmware will run it: an int8 input of
  `[1, 3, 40]` every 30 ms, and a uint8 probability out. We don't score the float model, the
  non-streaming model, or Keras.
- **The same feature frontend as the device.** That is the TFLM `micro_speech` frontend
  (`pymicro_features`, the same C code): 16 kHz mono int16, 30 ms window, 10 ms step, 40
  channels, 125-7500 Hz, PCAN + noise reduction on. Features stream continuously across a whole
  background track. The frontend is not reset per 10 s clip.
- **The same decision rule as the firmware.** `detect` fires when the mean of the last
  `sliding_window_size` uint8 outputs, divided by 255, is `> probability_cutoff`. After a
  detection, the window is cleared and the next **25 inferences (0.75 s)** are ignored. This is
  upstream microWakeWord's cooldown. It's shorter than the wake turn the firmware will start, so
  it can only over-count false accepts, never under-count them.

## Splits (nothing in test is ever trained on or tuned on)

All positives and hard negatives are synthetic (Piper TTS). There are no human recordings.

| Split | TTS voices | Synthesis settings | Augmentation (RIR / noise) | Role |
|---|---|---|---|---|
| train | LibriTTS-R generator: 660 speakers (seeded shuffle of ids 0-799); 13 stock Piper voices; ~70 % of the speakers of vctk / l2arctic / arctic / aru / semaine | speed 0.75-1.4, noise 0.5-1.0, noise_w 0.5-1.0, SLERP 0-1 | `rirs_train` (~80 % of MIT RIRs); noise = AudioSet bal_train 00-01 + FMA-xs; SNR -5..10 dB | fit weights |
| val | LibriTTS-R: 60 other speakers; ~10 % of the multi-speaker voices' speakers | same ranges as train | same pools as train | checkpoint choice, cutoff + window choice, iteration decisions |
| test | LibriTTS-R: 80 other speakers; **10 stock Piper voices never used in train/val**; ~20 % of the multi-speaker voices' speakers | **values never used in train/val**: speed {0.8, 0.93, 1.18, 1.33}, noise {0.6, 0.72, 0.95}, noise_w {0.65, 0.9}, SLERP {0.35, 0.6} | **`rirs_test` (the other ~20 % of MIT RIRs) + AudioSet bal_train shard 02 (never used anywhere else)** | the reported numbers |

Speaker/voice disjointness and settings disjointness come from code (`train/generate.py`:
`LT_SPLIT`, `ONNX_SINGLE`, `ONNX_MULTI`, `SETTINGS`). They aren't a convention we promise to
follow by hand.

### Test conditions for B1 (FRR)

Each held-out positive sits in a 3.2 s clip, with the phrase ending about 0.2 s before the end.
Detection is scored on the whole clip (any detection = accept).

- **B1 condition, "noisy":** reverb with a test RIR at p = 0.5; test background noise at p = 0.75
  with SNR uniform in **[5, 15] dB**; gain -20..0 dB; seeded.
- Also reported, outside the bar: **"clean"** (no augmentation) and **"hard"** (RIR p = 1,
  noise p = 1, SNR [0, 5] dB).

### Background for B2 (FA/h): never trained on, never used for tuning

| Source | What it is | Duration |
|---|---|---|
| DipCo (`dinner_party_eval/testing_ambient`, precomputed by microWakeWord) | far-field dinner-party conversation, 4 people | 5.33 h |
| AudioSet *evaluation* split, parquet shards 00-05, concatenated into continuous tracks | everyday sound: speech, TV, music, household, outdoor | 6 × 500 clips ≈ 8.3 h (exact figure recorded in `metrics.json`) |

The training negatives are microWakeWord's `speech` (VOiCES), `dinner_party` (CHiME-6 *train*)
and `no_speech` (FMA-medium, FSD50K, WHAM) sets, plus our TTS hard negatives. None of them
overlaps the DipCo or AudioSet-evaluation audio. The hours actually evaluated are reported, and
if they come in under the planned amount, that is stated.

### Validation (used for every choice)

- Positives and hard negatives from the val speakers. In training, they're augmented like
  train. For the operating-point choice, they use the "noisy" condition's parameters (p, SNR,
  gain), but drawn from the **train** RIR/noise pools. Validation never touches a test pool.
- Ambient: CHiME-6 dev+eval (`dinner_party_eval/validation_ambient`, 9.67 h) + AudioSet
  evaluation shards 30-31 (2 × 500 clips ≈ 2.8 h).

## Operating-point rule (validation only)

1. For each `sliding_window_size` W in {3, 5, 7, 10}, take the **smallest** `probability_cutoff`
   c on the grid 0.50, 0.51, ..., 0.99 whose validation FA/h is **≤ 0.5**. That is half of B2,
   as margin for the distribution shift to the test background.
2. Pick the W with the lowest validation FRR (noisy condition) at its c. On a tie, take the
   larger W.
3. Freeze (c, W). Run the test split once at (c, W) and report it.

The training checkpoint ("best weights") is also picked on validation only. That is
microWakeWord's own selection, run on the `validation` / `validation_ambient` sets.

## Iterating

When a model misses a bar, we change the **data** (more voices, harder or more negatives, more
augmentation) or the training schedule. The decision to iterate, and what to change, comes from
validation numbers. Test numbers are still computed and reported for **every** iteration, so the
history hides nothing. Every iteration is logged in `firmware/wakeword/README.md` (what changed,
then the validation and test metrics). We never loosen a bar, and we never put test speakers,
test settings, test RIRs/noise or test background audio into training.

## Also reported (no bar)

- FA rate on the held-out **hard negatives**, per phrase (fraction of clips that trigger). This
  covers the task's explicit confusables: "hey whisper", "a vesper", "vespers", "Vespa",
  "best for", "hey vest", "hey Esther", bare "Vesper", plus extras.
- FA/h per background source.
- Dataset counts per class and split.

## Caveat that no synthetic number removes

Every positive here is TTS. Real voices (Kevin's, family's), the AiPi Lite's own microphone and
the room are not in any of these sets. Passing B1-B3 means the model is worth putting on the
board. It does not prove the model works on the board. The investigation doc's plan to validate
with real recordings captured through the serial console still stands, as the step after this
task.
