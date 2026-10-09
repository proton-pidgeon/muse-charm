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
(Caveat, added 2026-10-09: the original test background was scored in all 26 runs, so it is development-consumed; see the honesty notes in `README.md` and addendum #3, which scores a fresh holdout once.)

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

(Caveat, added 2026-10-09: "never used for tuning" holds for training and for the operating-point rule, but these test numbers were visible for every one of the 26 runs, so this background is development-consumed. Addendum #3 below adds a fresh confirmatory holdout.)

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
  evaluation shards 30-31 (2 × 500 clips ≈ 2.8 h); from it14, also shards 06-15 (addendum #2
  below, ≈ 13.7 h more).

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

### Choosing which iteration ships (addendum, 2026-10-09, after it8 and before it9 was scored)

The doc above didn't say how to pick among iterations, so this rule fixes it. It uses validation
only, like everything else here. The shipped model is the iteration with the **lowest
validation FRR (noisy) at its own validation-chosen operating point**. On a tie, it's the one
with the lower validation FA/h. The test numbers of every iteration are reported, but they never
pick the winner. If that model fails a bar on test, the task fails. We don't go and look for an
iteration that happens to pass on test.

### Addendum #2 (2026-10-09, committed before it14 was trained or scored)

After 19 runs, the validation FA/h (tuned to 0.40-0.48) under-read the test FA/h (0.66-2.06)
every time (`README.md`, *What the history says*). The validation ambient was 12.4 h, 9.7 h of
it CHiME-6 dev/eval, the same corpus as a training negative; its AudioSet part was 2.8 h, too
little to constrain anything (0 false accepts in it meant "fewer than ~0.36 / h"). This addendum
changes the **validation** data and the **validation-only** rule. It changes nothing about the
bars, the test sets, or the "never tune on test" principle. It was written before any run with
mined negatives existed, so no test number of a new run informed it.

1. **Bigger validation ambient.** AudioSet *evaluation* shards 06-15 (10 × 500 clips, ≈ 13.7 h)
   join the validation ambient, next to CHiME-6 dev+eval (9.67 h) and shards 30-31 (2.76 h):
   ≈ 26 h in total, 16.5 h of it AudioSet. They are disjoint from the test shards (00-05), from
   the test noise pool (bal_train 02) and from every training negative (bal_train 00-01, 03-37;
   unbal_train 000-005 in it10-it13). Nothing from them is ever trained on.
2. **Per-family constraint.** Step 1 of the operating-point rule becomes: for each W, the
   smallest cutoff whose validation FA/h is ≤ 0.5 on the **total** *and* on **each source
   family** separately (family = CHiME-6 dev/eval; family = AudioSet evaluation). A cutoff
   that is safe on average but fails one family doesn't qualify. Steps 2-3 are unchanged.
3. **Hard-negative mining is training data.** `train/mine_negatives.py` runs a trained model
   over the *training* negative sets only (VOiCES, CHiME-6 train, FMA, FSD50K, WHAM, AudioSet
   bal_train 03-37, the train noise pool) and stores the windows where it fires or nearly
   fires as an extra, end-aligned negative set. It never reads the validation ambient, the
   test background, the test pools or any held-out TTS split (the source list is in the
   script). Which model is scanned, and the threshold, are iteration choices made on
   validation numbers.
4. **Which iteration ships (amended).** Every run, old (it1-it13) and new, is re-scored under
   items 1-2. The shipped model is the run with the **lowest validation FRR (noisy) at its own
   amended operating point**; tie → the lower validation FA/h; still tied → the larger W. Test
   numbers are reported for every run and never pick the winner. If the chosen run fails a bar
   on test, the task fails, exactly as before.

The 19 old runs' validation numbers under the old rule stay in the README as history.

### Addendum #3: confirmatory fresh holdout (2026-10-09, committed before the data was downloaded or scored)

The adjudicator noted that the test background (DipCo + AudioSet eval shards 00-05) was looked at
in 26 runs. This addendum scores the final model once on audio no run has ever touched. It
changes no bar and no earlier number.

1. **Model and point.** The committed `firmware/wakeword/hey-vesper.tflite` (sha256
   `40b3510d094de7fd2a8848faad09573915ad86b291381317403b09e419b6340d`), at the frozen point
   cutoff 0.65 / window 3 (`eval.py --fixed-cutoff 0.65 --fixed-window 3`).
2. **Data.** AudioSet *evaluation* parquet shards 16-29 (HF `agkphysics/AudioSet`, `data/eval`),
   each concatenated into a continuous track and run through the same feature pipeline as the
   test background (`background_confirm` in `download_data.py`; `eval.py --confirm-only`). Shards
   16-29 are used by no training, validation or test run (the used evaluation shards are 00-15
   and 30-31). Scored exactly once. If fewer than 14 shards fit on disk, the shards from 16
   upward that were scored are stated.
3. **No new positives.** FRR stays as reported on the existing test set.
4. **Pass criterion.** Total FA/h **< 1.0** over the fresh hours. The result is reported whichever
   way it goes, with per-shard counts and a Poisson 95 % upper bound. No retraining, re-tuning
   or cutoff change in response to it.

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
