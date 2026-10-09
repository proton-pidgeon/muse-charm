# firmware/wakeword/: the "Hey Vesper" microWakeWord model (task 21)

> **Status: this model meets the three quality bars on the held-out test.** At the operating
> point chosen on validation alone (cutoff 0.65, window 3): false-reject rate **2.93 %** (bar
> < 5 %), **0.22 false accepts per hour** on 13.58 h of held-out background (bar < 1 / h),
> **60,840 bytes** (bar ≤ 65,536). It is run it14 with seed 22, the 21st of 26 training runs
> (see *Iteration history*), picked by the validation-only rule in `quality-bars.md`
> (addendum #2), not by the test. Swapping it into the firmware is the separate step after
> Kevin hears the numbers (task step 6). Real voices and the board's own microphone are still
> untested; see the caveat at the end of `quality-bars.md`.

| File | What |
|---|---|
| `hey-vesper.tflite` | int8 streaming microWakeWord model, 60,840 bytes, sha256 `40b3510d…6340d` |
| `metrics.json` | machine-readable: model I/O, operating point, every test/validation metric, dataset counts, all 26 runs |
| `quality-bars.md` | the bars + evaluation protocol, committed **before** the first training run, plus the two dated addenda to the selection rule |
| `train/` | the full recipe (scripts, configs, pinned locks, README with one command per stage) |

## Integration note (for the firmware swap, task step 6)

| Item | Value |
|---|---|
| Audio in | 16 kHz, mono, int16. That's the existing `idle_capture()` feed (`MUSE_AUDIO_RATE 16000`, 20 ms = 320-sample chunks) |
| Feature frontend | TFLM `micro_speech` audio frontend, the same C code as ESPHome `micro_wake_word` and `pymicro_features`: **30 ms window, 10 ms step, 40 mel channels**, 125-7500 Hz, noise reduction (smoothing bits 10, even 0.025, odd 0.06, min signal 0.05), PCAN on (strength 0.95, offset 80, gain bits 21), log scale (shift 6). Output: 40 uint16 values per 10 ms. Each 20 ms mic chunk yields 2 feature frames |
| Stride | **3 feature frames per inference**, so one inference every **30 ms** (every 1.5 mic chunks: count frames and invoke when 3 new ones are in) |
| Input tensor | `[1, 3, 40]` **int8**, scale 0.10196078 (26/255), zero point -128. Per uint16 feature `f`: `q = clamp(round(f / 25.6 / 0.10196078) - 128, -128, 127)`. Feed the 3 frames oldest first |
| Output tensor | `[1, 1]` **uint8**, scale 1/256, zero point 0. `p = q / 255` (the ESPHome convention; the eval uses it too) |
| Streaming state | **internal** (`stream_state_internal_quant`). The model keeps its ring buffers in resource variables, so the firmware only feeds new frames. The op resolver needs: `CONV_2D, DEPTHWISE_CONV_2D, FULLY_CONNECTED, LOGISTIC, RESHAPE, STRIDED_SLICE, SPLIT_V, CONCATENATION, QUANTIZE, CALL_ONCE, VAR_HANDLE, READ_VARIABLE, ASSIGN_VARIABLE` (13 ops; 2 subgraphs) |
| Decision rule | keep the last **W = 3** uint8 outputs. Detect when `sum > round(0.65 × 255) × 3 = 166 × 3 = 498` (that is, `probability_cutoff = 0.65`, `sliding_window_size = 3`). After a detection, clear the window and ignore the next 25 inferences (0.75 s). Also ignore the first 25 inferences after the frontend and model are (re)started. This is exactly what `train/eval.py` scored |
| Tensor arena | **ESTIMATE ≈ 31 KB** (30,842 B). Non-constant tensors are 17,963 B, plus the TFLM overhead that stock `hey_jarvis` shows (9,981 B of tensors → 22,860 B arena in ESPHome's manifest). This model has 2 more depthwise convs and `SPLIT_V`. Allocate ~36 KB and read the real figure from `arena_used_bytes()` on the board. The investigation doc's estimate (23-26 KB) was for the stock models |
| Model size | 60,840 B, embedded in the app image (no partition change) |

The architecture (upstream mixednet, stride 3) is unchanged from the first run, so the
firmware-side contract above is the same for every model in the history; only the cutoff moved.

## Quality bars (fixed before training; see `quality-bars.md`)

| # | Bar | This model (test, at 0.65 / W 3) | Pass |
|---|---|---|---|
| B1 | FRR < 5 % on held-out positives, "noisy" condition | **2.93 %** (88 of 3,000 missed) | ✓ |
| B2 | FA < 1 per hour on held-out background | **0.22 / h** (3 false accepts in 13.58 h) | ✓ |
| B3 | model ≤ 65,536 B | **60,840 B** | ✓ |

### Final metrics (shipped model = run it14, seed 22)

| Metric | Validation (chooses everything) | Test (held out) |
|---|---|---|
| FRR, noisy (bar condition) | 2.53 % (n = 1,500) | **2.93 %** (n = 3,000) |
| FRR, clean / hard | n/a | 1.90 % / 9.00 % |
| FRR by voice source (noisy) | LibriTTS-R speakers 0.6 %, Piper multi-speaker voices 6.4 % | held-out LibriTTS-R speakers 1.00 %, **unseen Piper voices/speakers 4.87 %** |
| FA / h | 0.38 (10 in 26.18 h: CHiME-6 dev+eval 3 / 9.67 h = 0.31; AudioSet eval 06-15 + 30-31 7 / 16.50 h = 0.42) | **0.22** (3 in 13.58 h: DipCo 2 / 5.34 h = 0.37; AudioSet eval 00-05 1 / 8.25 h = 0.12) |
| FA / h, fresh confirmatory holdout (addendum #3) | n/a | **0.31** (6 in 19.23 h, AudioSet eval 16-29; Poisson 95 % upper 0.68 / h; untouched by all 26 runs) |
| TTS hard-negative trigger rate (any detection in the clip) | 9.8 % | 10.6 % (n = 3,520; table below) |
| General TTS speech trigger rate | n/a | 0.8 % (n = 600) |

**Confirmatory fresh holdout.** The test background above was scored in all 26 runs, so it is
development-consumed (see the honesty notes). To remove that caveat, `quality-bars.md` addendum
#3 was committed (`8940347`) before anything was downloaded: score the shipped model once, at the
frozen point (0.65 / W 3), on AudioSet evaluation shards 16-29, which no run had used. Result:
**6 false accepts in 19.23 h = 0.31 / h** (bar < 1.0; Poisson 95 % upper bound 0.68 / h), so
**it passes**. Per shard: 22 and 27 had 2 each, 17 and 20 had 1 each, the other ten had 0. No
retraining or tuning followed. This covers background audio only; FRR is unchanged (no new
positives). Details in `metrics.json` (`confirmatory_holdout`).

Background evaluated: **13.58 h** of held-out test audio (DipCo 5.34 h + AudioSet evaluation
shards 00-05, 8.25 h), never trained on and never tuned on. Validation background: 26.18 h
(addendum #2; it was 12.43 h for it1-it13's original scoring).

The validation selection curves (per W, the smallest cutoff with ≤ 0.5 FA/h on the total and on
each family): W 3 → 0.65 (FRR 2.53 %), W 5 → 0.64 (3.67 %), W 7 → 0.61 (6.07 %), W 10 → 0.54
(11.0 %). W 3 wins. The test trade-off curve at W = 3, reported and never used to choose
anything:

| cutoff | 0.50 | 0.55 | 0.60 | **0.65** | 0.70 | 0.75 | 0.80 | 0.85 | 0.90 |
|---|---|---|---|---|---|---|---|---|---|
| test FRR (noisy) | 2.23 % | 2.43 % | 2.63 % | **2.93 %** | 3.70 % | 4.23 % | 4.87 % | 6.47 % | 8.90 % |
| test FA / h | 0.74 | 0.52 | 0.52 | **0.22** | 0.15 | 0.15 | 0.15 | 0.07 | 0.00 |

### Hard negatives (held-out speakers/voices, noisy condition, 80 clips each)

The task's required confusables:

| phrase | "hey whisper" | "a vesper" | "vespers" | "Vespa" | "best for" | "hey vest" | "hey Esther" | bare "Vesper" |
|---|---|---|---|---|---|---|---|---|
| trigger rate | 8.8 % | 13.8 % | 0.0 % | 0.0 % | 0.0 % | 1.3 % | 5.0 % | 0.0 % |

The worst of our extra confusables: "hey Vespa" 89 %, "hey vespers" 70 %, "hey, vest pocket"
63 %, "hey vessel" 49 %, "hey, best friend" 40 %, "hey Vesta" 31 %, "hey Esper" 16 %, "evening
vespers" 14 %. For "hey Vespa" and "hey vespers", TTS output barely differs from "hey vesper" at
all. Every phrase is in `metrics.json` (`test.hardneg_per_phrase`). These are TTS clips of the
confusable said in isolation, not a rate per hour; the mining stage targeted real-audio false
accepts, not these.

## Dataset composition (the shipped model's data)

All speech is synthetic (Piper). No human recordings. Speaker/voice disjointness is enforced in
`train/generate.py`.

| Class | Split | LibriTTS-R gen. (`lt`) | stock EN Piper (`onnx`) | non-EN Piper, EN phonemes (`xl` + `xl2`) | libritts-high (`lh`) | total clips |
|---|---|---|---|---|---|---|
| "hey vesper" (positive) | train | 40,000 (660 spk) | 15,000 | 14,275 + 14,759 (63 voices) | 15,000 (train spk only) | 99,034 |
| | val | 1,000 (60 other spk) | 500 | - | - | 1,500 |
| | test | 1,500 (80 other spk) | 1,500 (10 unseen voices + held-out multi-spk) | - | - | 3,000 |
| hard negatives (44 phrases) | train | 17,600 | 6,600 | 4,232 + 4,259 | 4,400 | 37,091 |
| | val | 1,100 | 440 | - | - | 1,540 |
| | test | 1,760 | 1,760 | - | - | 3,520 |
| general TTS speech (50 sentences) | train | 3,000 | 1,500 | 771 + 829 | 1,000 | 7,100 |
| | val / test | 150 / 300 | 50 / 300 | - | - | 200 / 600 |

Training spectrograms: 198,068 positive (2 augmented copies per clip, each shifted 0-9 frames
during training), 74,182 hard-negative + 14,200 general (2 copies). Precomputed real-audio
negatives (training only): VOiCES far-field speech 162 h, CHiME-6 train (array u01) 40.5 h,
FMA-medium music 173 h, FSD50K 114 h, WHAM noise 43 h. Ours: AudioSet bal_train 03-37 plus the
train noise pool, 51.6 h (18,393 clips). Augmentation (train/val pools only): MIT RIRs (217 of
270), AudioSet bal_train 00-01 + FMA-xs as background at SNR -5..10 dB, plus the upstream
EQ/distortion/pitch/band-stop/colour-noise/gain mix.

**Mined negatives (`mined_neg_r1`, new in it14):** 6,814 end-aligned 224-frame windows cut
from those same training negative sets at the places where seven earlier models (it8, it1,
it5, it9, it9b, it12s22, it11s22) fired or nearly fired (streaming output mean over W = 3
≥ 0.5; scans at ≥ 0.3, 534 h scanned per model, 26,627 raw hits, duplicates within 30 frames
merged). By source: VOiCES 1,547; CHiME-6 train 449; FMA-medium 1,440; FSD50K no-speech 1,195;
FSD50K speech 497; WHAM 73; train noise pool 122; AudioSet bal_train 03-37 1,491. Sampled at
weight 6 (of 48) with `fixed_right_cutoff` 0-9, like the aligned TTS hard negatives. Nothing
from validation or test is in it (`train/mine_negatives.py`, `SOURCES`).

## Iteration history

The bars and protocol were committed first (`81baf49`). Each row is one training run, scored by
`train/eval.py`, with the operating point chosen on validation. "Bars" = B1 B2 B3.

### Under addendum #2 (validation ambient 26.2 h, per-family FA/h ≤ 0.5): all 26 runs

This is the table the selection rule reads. it1-it13 were re-scored under it (same models,
same test audio; only the validation ambient and the cutoff constraint changed). Val FA/h is
followed by the two family rates (ch = CHiME-6 dev/eval, au = AudioSet eval 06-15 + 30-31); test
FA/h by the per-family rates (di = DipCo, au = AudioSet eval 00-05).

| Run | Val FRR (noisy) | Val FA/h | cutoff / W | Test FRR noisy (clean / hard) | Test FA/h (FAs / h) | Test hard-neg trigger | Bars |
|---|---|---|---|---|---|---|---|
| it1 | 4.53 % | 0.38 (ch 0.21, au 0.48) | 0.88 / 3 | 6.20 % (4.3 / 13.0) | 0.74 (10 / 13.58; di 0.75, au 0.73) | 10.7 % | ✗✓✓ |
| it2 | 16.60 % | 0.34 (ch 0.10, au 0.48) | 0.90 / 3 | 18.57 % (12.6 / 29.7) | 0.29 (4 / 13.58; di 0.19, au 0.36) | 2.1 % | ✗✓✓ |
| it3 | 12.60 % | 0.42 (ch 0.31, au 0.48) | 0.68 / 3 | 13.03 % (7.5 / 24.1) | 0.81 (11 / 13.58; di 0.56, au 0.97) | 4.8 % | ✗✓✓ |
| it4 | 12.73 % | 0.27 (ch 0.10, au 0.36) | 0.95 / 3 | 13.57 % (9.8 / 23.1) | 0.81 (11 / 13.58; di 1.50, au 0.36) | 2.0 % | ✗✓✓ |
| it5 | 5.73 % | 0.42 (ch 0.41, au 0.42) | 0.88 / 3 | 7.57 % (4.5 / 15.6) | 0.52 (7 / 13.58; di 0.37, au 0.61) | 6.6 % | ✗✓✓ |
| it6a | 7.27 % | 0.34 (ch 0.10, au 0.48) | 0.88 / 3 | 7.37 % (4.3 / 15.3) | 1.10 (15 / 13.58; di 0.94, au 1.21) | 6.0 % | ✗✗✓ |
| it6b | 10.40 % | 0.34 (ch 0.10, au 0.48) | 0.92 / 3 | 10.70 % (7.3 / 20.6) | 0.59 (8 / 13.58; di 0.56, au 0.61) | 2.7 % | ✗✓✓ |
| it7 | 8.07 % | 0.42 (ch 0.41, au 0.42) | 0.81 / 3 | 7.50 % (4.8 / 17.2) | 0.74 (10 / 13.58; di 0.37, au 0.97) | 5.1 % | ✗✓✓ |
| it8 | 5.20 % | 0.42 (ch 0.31, au 0.48) | 0.88 / 3 | 6.23 % (3.3 / 15.0) | 0.88 (12 / 13.58; di 1.12, au 0.73) | 8.8 % | ✗✓✓ |
| it9 | 5.93 % | 0.38 (ch 0.21, au 0.48) | 0.78 / 3 | 6.13 % (4.4 / 14.2) | 0.66 (9 / 13.58; di 0.75, au 0.61) | 6.1 % | ✗✓✓ |
| it9b | 8.33 % | 0.34 (ch 0.10, au 0.48) | 0.75 / 3 | 8.20 % (6.0 / 15.5) | 0.88 (12 / 13.58; di 0.75, au 0.97) | 7.2 % | ✗✓✓ |
| it10 | 13.07 % | 0.38 (ch 0.21, au 0.48) | 0.91 / 3 | 12.97 % (8.8 / 21.6) | 0.59 (8 / 13.58; di 0.75, au 0.48) | 2.2 % | ✗✓✓ |
| it11 s22 | 6.40 % | 0.38 (ch 0.21, au 0.48) | 0.90 / 3 | 7.10 % (3.4 / 17.0) | 0.74 (10 / 13.58; di 0.75, au 0.73) | 5.4 % | ✗✓✓ |
| it11 s23 | 10.27 % | 0.42 (ch 0.31, au 0.48) | 0.87 / 3 | 10.70 % (8.0 / 17.7) | 0.88 (12 / 13.58; di 0.56, au 1.09) | 2.8 % | ✗✓✓ |
| it11 s24 | 6.40 % | 0.38 (ch 0.31, au 0.42) | 0.95 / 3 | 8.00 % (4.1 / 18.6) | 1.03 (14 / 13.58; di 0.94, au 1.09) | 9.8 % | ✗✗✓ |
| it12 s21 | 7.20 % | 0.42 (ch 0.31, au 0.48) | 0.87 / 5 | 7.30 % (4.3 / 14.3) | 1.47 (20 / 13.58; di 1.31, au 1.58) | 3.8 % | ✗✗✓ |
| it12 s22 | 6.93 % | 0.34 (ch 0.10, au 0.48) | 0.85 / 3 | 7.77 % (4.8 / 16.0) | 0.59 (8 / 13.58; di 0.56, au 0.61) | 4.7 % | ✗✓✓ |
| it13 s21 | 9.33 % | 0.42 (ch 0.31, au 0.48) | 0.77 / 5 | 9.30 % (7.0 / 16.7) | 0.66 (9 / 13.58; di 0.75, au 0.61) | 5.7 % | ✗✓✓ |
| it13 s22 | 8.87 % | 0.31 (ch 0.21, au 0.36) | 0.92 / 3 | 8.47 % (5.5 / 16.3) | 1.10 (15 / 13.58; di 1.50, au 0.85) | 3.4 % | ✗✗✓ |
| it14 s21 | 5.13 % | 0.23 (ch 0.10, au 0.30) | 0.50 / 3 | 5.83 % (5.4 / 11.8) | 0.52 (7 / 13.58; di 0.75, au 0.36) | 11.8 % | ✗✓✓ |
| **it14 s22** | **2.53 %** | 0.38 (ch 0.31, au 0.42) | 0.65 / 3 | **2.93 % (1.9 / 9.0)** | **0.22 (3 / 13.58; di 0.37, au 0.12)** | 10.6 % | **✓✓✓** |
| it15 | 5.47 % | 0.42 (ch 0.41, au 0.42) | 0.67 / 7 | 4.67 % (3.0 / 10.0) | 0.96 (13 / 13.58; di 1.50, au 0.61) | 6.7 % | ✓✓✓ |
| it16 | 6.20 % | 0.42 (ch 0.41, au 0.42) | 0.68 / 3 | 6.27 % (3.9 / 11.8) | 1.33 (18 / 13.58; di 1.12, au 1.45) | 4.5 % | ✗✗✓ |
| it17 | 5.87 % | 0.38 (ch 0.21, au 0.48) | 0.80 / 3 | 6.07 % (3.8 / 12.7) | 0.52 (7 / 13.58; di 0.56, au 0.48) | 6.0 % | ✗✓✓ |
| it18 | 3.47 % | 0.38 (ch 0.21, au 0.48) | 0.59 / 3 | 4.07 % (2.3 / 8.5) | 0.59 (8 / 13.58; di 0.56, au 0.61) | 7.0 % | ✓✓✓ |
| it19 | 3.87 % | 0.42 (ch 0.31, au 0.48) | 0.68 / 3 | 4.17 % (2.4 / 9.5) | 0.22 (3 / 13.58; di 0.00, au 0.36) | 6.1 % | ✓✓✓ |

What changed in it14-it19 (configs in `train/iterations/`; everything before is below):

- **it14** (seed 21): it8's data + the mined negative set `r1` at weight 6 (`04b_mine.sh`,
  see *Dataset composition*). Val FA/h fell to 0.23 at the grid's lowest cutoff (0.50), so the
  rule couldn't trade the headroom for recall; val FRR 5.13 %, nearly all of it on the Piper
  multi-speaker val voices (12.8 %; LibriTTS-R 1.3 %).
- **it14 s22**: the same config, seed 22. Val FRR 2.53 %, the best of all 26 runs, at cutoff
  0.65 → **shipped** under the rule. The seed swing (5.13 → 2.53 %) is larger than any data
  change in this history; see the caveats.
- **it15**: it14 + it12's longer plateau (positives and aligned hard negatives shifted 0-20
  frames). Val FRR 5.47 % at W = 7. Passes the bars on test, but lost on validation.
- **it16 / it17**: the mined set at weight 2 / weight 3 with positives at 6. Both worse on
  validation (6.2 % / 5.9 %).
- **it18 / it19**: fine-tunes of it8's final weights (`WW_INIT_FROM=it8`, 10k steps at
  LR 2e-4) on it14's data, mined weight 6 / 3. Val FRR 3.47 % / 3.87 %; both pass the bars on
  test; both lost on validation to it14 s22.
- Scanning the training negatives again with it14 and it15 found almost nothing new (31 and
  360 hits ≥ 0.5 vs 2,073 for it8), so a second mining round wasn't built.

### Under the original rule (validation ambient 12.4 h): it1-it13, as first scored

| Run | Val FRR (noisy) | Val FA/h | cutoff / W | Test FRR noisy (clean / hard) | Test FA/h (FAs / h) | Test hard-neg trigger | Bars |
|---|---|---|---|---|---|---|---|
| it1 | 3.40 % | 0.48 | 0.83 / 3 | 4.97 % (3.6 / 10.3) | 1.33 (18 / 13.58) | 12.4 % | ✓✗✓ |
| it2 | 11.27 % | 0.48 | 0.83 / 3 | 13.40 % (8.3 / 23.0) | 0.66 (9 / 13.58) | 2.9 % | ✗✓✓ |
| it3 | 11.93 % | 0.48 | 0.65 / 3 | 12.50 % (7.0 / 23.0) | 0.88 (12 / 13.58) | 5.1 % | ✗✓✓ |
| it4 | 6.60 % | 0.40 | 0.87 / 3 | 7.63 % (5.3 / 13.7) | 1.69 (23 / 13.58) | 3.6 % | ✗✗✓ |
| it5 | 4.33 % | 0.48 | 0.83 / 3 | 5.57 % (2.8 / 13.0) | 1.18 (16 / 13.58) | 8.0 % | ✗✗✓ |
| it6a | 6.40 % | 0.48 | 0.86 / 3 | 6.53 % (3.9 / 13.5) | 1.18 (16 / 13.58) | 6.7 % | ✗✗✓ |
| it6b | 6.87 % | 0.48 | 0.87 / 3 | 7.07 % (5.0 / 15.0) | 0.96 (13 / 13.58) | 4.0 % | ✗✓✓ |
| it7 | 5.80 % | 0.48 | 0.74 / 3 | 5.63 % (3.8 / 13.7) | 1.25 (17 / 13.58) | 6.3 % | ✗✗✓ |
| it8 | 3.27 % | 0.48 | 0.80 / 3 | 3.83 % (2.2 / 10.2) | 1.55 (21 / 13.58) | 12.0 % | ✓✗✓ |
| it9 | 3.87 % | 0.40 | 0.66 / 3 | 3.37 % (2.0 / 9.4) | 1.69 (23 / 13.58) | 8.5 % | ✓✗✓ |
| it9b | 5.00 % | 0.48 | 0.58 / 3 | 4.70 % (3.6 / 9.6) | 2.06 (28 / 13.58) | 10.8 % | ✓✗✓ |
| it10 | 7.47 % | 0.48 | 0.82 / 3 | 7.37 % (5.1 / 13.2) | 1.91 (26 / 13.58) | 3.7 % | ✗✗✓ |
| it11 s22 | 5.20 % | 0.48 | 0.85 / 3 | 5.33 % (2.6 / 13.5) | 1.33 (18 / 13.58) | 6.8 % | ✗✗✓ |
| it11 s23 | 7.27 % | 0.48 | 0.81 / 3 | 7.53 % (6.0 / 13.3) | 1.62 (22 / 13.58) | 3.7 % | ✗✗✓ |
| it11 s24 | 5.60 % | 0.48 | 0.94 / 3 | 7.27 % (3.8 / 17.0) | 1.10 (15 / 13.58) | 10.7 % | ✗✗✓ |
| it12 s21 | 6.40 % | 0.48 | 0.88 / 3 | 6.93 % (3.9 / 13.3) | 1.47 (20 / 13.58) | 4.6 % | ✗✗✓ |
| it12 s22 | 3.53 % | 0.48 | 0.67 / 3 | 3.33 % (1.9 / 7.8) | 1.47 (20 / 13.58) | 8.3 % | ✓✗✓ |
| it13 s21 | 7.67 % | 0.40 | 0.74 / 3 | 7.40 % (5.4 / 13.3) | 1.10 (15 / 13.58) | 7.5 % | ✗✗✓ |
| it13 s22 | 7.13 % | 0.48 | 0.90 / 3 | 6.83 % (4.6 / 14.6) | 1.25 (17 / 13.58) | 4.2 % | ✗✗✓ |

Under that rule it8 shipped (lowest val FRR) and failed B2 on test (1.55 / h). That is what
led to addendum #2.

What changed in each of it1-it13:

- **it1**: upstream notebook recipe. 16k LibriTTS-R + 6k stock-voice positives (1 augmented copy
  × 10 stored slides), our TTS hard negatives at random positions, and microWakeWord's speech /
  dinner-party / no-speech negatives. 20k steps. Hard negatives triggered often ("hey Vespa"
  78 %), and val FA was right at the limit.
- **it2**: hard negatives aligned exactly like the positives (`fixed_right_cutoff` 0-9) +
  AudioSet bal_train 03-12 negatives + a low-LR second phase (20k + 10k). The validation hard-negative trigger rate dropped to 1.4 %. Val FRR rose to 11 %, mostly on accented multi-speaker voices.
- **it3**: positives 2.5× (40k + 15k), stored once and shifted at train time, 2 augmented copies
  each. Val FRR unchanged.
- **it4**: positive sampling weight 2 → 4, aligned hard negatives 6 → 3. Val FRR 6.6 %.
- **it5**: + 15k positives from 28 non-English Piper voices fed English phonemes (accented
  English; train only), with hard negatives and general speech from the same voices. Val FRR
  4.3 %; the val FRR on held-out accented speakers fell from 15 % to 11 %.
- **it6a / it6b**: 2× steps, without / with SpecAugment. Neither helped on validation.
- **it7**: + 35 more non-English voices + `en_US-libritts-high` (a second Piper model of the
  LibriTTS speakers, train speaker indices only). No validation gain.
- **it8**: + AudioSet bal_train 13-37 as negatives (weight 5 → 8). Best validation FRR under
  the original rule (3.27 %); shipped then, failed B2 on test.
- **it9**: dinner-party (CHiME-6 train) weight 10 → 16. **it9b**: negative class weight
  20 → 30. Neither beat it8 on validation.
- **it10**: + AudioSet unbal_train 000-005 (~38 h) as negatives, CHiME-6 array u02 restored.
  Worse on validation.
- **it11 s22-s24**: it8's config on it10's data, three more seeds. Validation FRR spread over
  5.2-7.3 %, which shows that seed-to-seed variance is as large as most of the data effects
  above.
- **it12**: positives and aligned hard negatives shifted 0-20 frames (was 0-9), so a detection
  lasts ~200 ms instead of ~90 ms. Larger windows became usable: W = 10 val FRR went from
  15-55 % to 4.9 % (s22). The best seed reached 3.53 %, still behind it8.
- **it13**: CHiME-6 train weight 10 → 4, so validation (CHiME-6 dev/eval) is less optimistic.
  Worse.

### What the history says (and the caveats)

- **The validation/test FA gap was a validation-data problem.** Under the original 12.4 h
  validation ambient, every run was tuned to 0.40-0.48 FA/h and landed at 0.66-2.06 on test.
  Its AudioSet part (2.8 h) couldn't see anything (0 false accepts in it for it8, 1.2 / h on
  the test AudioSet). With 16.5 h of AudioSet in validation and the per-family constraint
  (addendum #2), the same models re-score at 0.27-0.42 on validation and 0.29-1.47 on test,
  and the mined-negative models at 0.23-0.42 / 0.22-1.33. CHiME-6 dev/eval still under-reads
  DipCo (0.31 vs 0.37 for the shipped model; 0.1-0.4 vs 0.2-1.5 across runs), so the margin
  is still needed.
- **Hard-negative mining moved the frontier.** The scan found 2,073 places in 534 h of
  training audio where it8 fired at ≥ 0.5 even though it had trained on that audio. Training
  on those windows end-aligned (weight 6) cut the shipped model's test FA/h to 0.22 at a cutoff
  (0.65) where it8 had 2.8 / h, while test FRR went from 3.83 % to 2.93 %. The mined set is
  small (6.8k windows) and gets memorised (it14 fires on 31 of them afterwards), so a second
  round from the same training audio has nothing left to find; new negative audio would be
  needed for another round.
- **Seed noise is still the largest single effect.** it14 seed 21 → 22 moved val FRR from
  5.13 % to 2.53 % with the same data; it11 showed 5.2-7.3 % across three seeds. The rule
  picks the lowest validation FRR, and that is what shipped. The test agrees (2.93 %, 0.22 / h)
  with margin on both bars, so this isn't a validation fluke that the test contradicts, but a
  rerun of the recipe should expect a model somewhere in the 2.5-5.5 % validation band and may
  need a second seed.
- **The misses are on voices the training never saw.** FRR on held-out LibriTTS-R speakers is
  ~1 %; on the unseen stock Piper voices and multi-speaker ids it is 4.9 % (test) / 6.4 %
  (val). That is where a bigger positive set (more voices) would still pay.
- **Honesty notes.** (1) Test numbers were computed and visible for every run, as the protocol
  says. it10-it13 went after the FA gap on the strength of test behaviour; addendum #2 was
  written and committed (`8777ec6`) before any mined-negative run was trained, and its two
  changes (more validation ambient, per-family constraint) came from the documented
  diagnosis, not from a test number of a new run. (2) The shipped model is the rule's pick;
  it15, it18 and it19 also pass on test but lost on validation and are not shipped. (3)
  `metrics.json` holds every run under the amended rule; this README keeps the original-rule
  numbers too.

### Suggested next steps (not done here)

1. **Real audio for validation.** Record Kevin and family saying "hey vesper", plus an evening
   of the actual room/TV, through the board's mic (serial console capture, as the investigation
   doc plans). Synthetic positives and YouTube/dinner-party negatives are a proxy for that.
2. Another mining round needs **new** negative audio (e.g. AudioSet unbal_train shards beyond
   005, or more far-field conversation), since the current training negatives are mined out.
3. More Piper voices (or a second TTS engine) for the positives, to close the 5-6 % FRR on
   unseen voices.

## Reproduce

See `train/README.md` for the details. On the Studio, from the repo root:

```sh
bash firmware/wakeword/train/01_setup.sh
bash firmware/wakeword/train/02_download.sh               # includes background_val_ext (addendum #2)
bash firmware/wakeword/train/03_generate.sh
bash firmware/wakeword/train/04_features.sh
# the mined set r1 = the union of 7 earlier models' scans; rebuild those models first
# (05_train.sh with their configs; it11s22 and it12s22 need WW_SEED=22 and it10's data, see train/README.md)
for r in it8 it1 it5 it9 it9b it12s22 it11s22; do
  bash firmware/wakeword/train/04b_mine.sh scan "$HOME/builds/muse-charm/scratch/wakeword-21/runs/$r/hey-vesper.tflite" "$HOME/builds/muse-charm/scratch/wakeword-21/runs/mine_$r.npz" --min-prob 0.3
done
bash firmware/wakeword/train/04b_mine.sh build r1 "$HOME"/builds/muse-charm/scratch/wakeword-21/runs/mine_{it8,it1,it5,it9,it9b,it12s22,it11s22}.npz --min-prob 0.5
WW_SEED=22 bash firmware/wakeword/train/05_train.sh final firmware/wakeword/train/iterations/it14.yaml
bash firmware/wakeword/train/06_eval.sh "$HOME/builds/muse-charm/scratch/wakeword-21/runs/final/hey-vesper.tflite" /tmp/final.json final
```

Re-score the committed model at its frozen point (about 1 minute with the eval cache):

```sh
bash firmware/wakeword/train/06_eval.sh firmware/wakeword/hey-vesper.tflite /tmp/shipped.json shipped --fixed-cutoff 0.65 --fixed-window 3
```

It reproduces the test numbers above exactly: FRR 2.93 % / 1.90 % / 9.00 %, 3 FA in 13.58 h.

Licence: training used CC-BY-NC negative data (microWakeWord's sets) and YouTube-sourced
AudioSet, so treat this model as personal / non-commercial, the same as upstream's notebook.
