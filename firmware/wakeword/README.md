# firmware/wakeword/: the "Hey Vesper" microWakeWord model (task 21)

> **Status: this model does NOT meet the quality bars. Do not swap it into the firmware.**
> On the held-out test, the false-reject rate passes (B1: 3.83 % < 5 %) and so does the size
> (B3: 60,840 B ≤ 65,536 B). The false-accept rate fails: **1.55 per hour** on 13.58 h of
> held-out background, where the bar is < 1 (B2). That is the honest result after 19 training
> runs (15 configurations; see *Iteration history*). The shipped iteration was chosen by the
> validation-only rule, not by the test. It is committed so it can be heard and tried on the
> bench, and so the next attempt starts from a measured baseline.

| File | What |
|---|---|
| `hey-vesper.tflite` | int8 streaming microWakeWord model, 60,840 bytes, sha256 `d2855efe…c595b5` |
| `metrics.json` | machine-readable: model I/O, operating point, every test/validation metric, dataset counts, all 19 runs |
| `quality-bars.md` | the bars + evaluation protocol, committed **before** the first training run |
| `train/` | the full recipe (scripts, configs, pinned locks, README with one command per stage) |

## Integration note (for the firmware step that comes after the bars are met)

| Item | Value |
|---|---|
| Audio in | 16 kHz, mono, int16. That's the existing `idle_capture()` feed (`MUSE_AUDIO_RATE 16000`, 20 ms = 320-sample chunks) |
| Feature frontend | TFLM `micro_speech` audio frontend, the same C code as ESPHome `micro_wake_word` and `pymicro_features`: **30 ms window, 10 ms step, 40 mel channels**, 125-7500 Hz, noise reduction (smoothing bits 10, even 0.025, odd 0.06, min signal 0.05), PCAN on (strength 0.95, offset 80, gain bits 21), log scale (shift 6). Output: 40 uint16 values per 10 ms. Each 20 ms mic chunk yields 2 feature frames |
| Stride | **3 feature frames per inference**, so one inference every **30 ms** (every 1.5 mic chunks: count frames and invoke when 3 new ones are in) |
| Input tensor | `[1, 3, 40]` **int8**, scale 0.10196078 (26/255), zero point -128. Per uint16 feature `f`: `q = clamp(round(f / 25.6 / 0.10196078) - 128, -128, 127)`. Feed the 3 frames oldest first |
| Output tensor | `[1, 1]` **uint8**, scale 1/256, zero point 0. `p = q / 255` (the ESPHome convention; the eval uses it too) |
| Streaming state | **internal** (`stream_state_internal_quant`). The model keeps its ring buffers in resource variables, so the firmware only feeds new frames. The op resolver needs: `CONV_2D, DEPTHWISE_CONV_2D, FULLY_CONNECTED, LOGISTIC, RESHAPE, STRIDED_SLICE, SPLIT_V, CONCATENATION, QUANTIZE, CALL_ONCE, VAR_HANDLE, READ_VARIABLE, ASSIGN_VARIABLE` (13 ops; 2 subgraphs) |
| Decision rule | keep the last **W = 3** uint8 outputs. Detect when `sum > round(0.80 × 255) × 3 = 204 × 3` (that is, `probability_cutoff = 0.80`, `sliding_window_size = 3`). After a detection, clear the window and ignore the next 25 inferences (0.75 s). Also ignore the first 25 inferences after the frontend and model are (re)started. This is exactly what `train/eval.py` scored |
| Tensor arena | **ESTIMATE ≈ 31 KB** (30,842 B). Non-constant tensors are 17,963 B, plus the TFLM overhead that stock `hey_jarvis` shows (9,981 B of tensors → 22,860 B arena in ESPHome's manifest). This model has 2 more depthwise convs and `SPLIT_V`. Allocate ~36 KB and read the real figure from `arena_used_bytes()` on the board. The investigation doc's estimate (23-26 KB) was for the stock models |
| Model size | 60,840 B, embedded in the app image (no partition change) |

The operating point (0.80 / 3) is what the validation-only rule picked for this model. If a
future model passes the bars, its own `metrics.json` carries its own point.

## Quality bars (fixed before training; see `quality-bars.md`)

| # | Bar | This model (test, at 0.80 / W 3) | Pass |
|---|---|---|---|
| B1 | FRR < 5 % on held-out positives, "noisy" condition | **3.83 %** (115 of 3,000 missed) | ✓ |
| B2 | FA < 1 per hour on held-out background | **1.55 / h** (21 false accepts in 13.58 h) | **✗** |
| B3 | model ≤ 65,536 B | **60,840 B** | ✓ |

### Final metrics (shipped model = run it8, seed 21)

| Metric | Validation (chooses everything) | Test (held out) |
|---|---|---|
| FRR, noisy (bar condition) | 3.27 % (n = 1,500) | **3.83 %** (n = 3,000) |
| FRR, clean / hard | n/a | 2.20 % / 10.20 % |
| FRR by voice source (noisy) | LibriTTS-R speakers 0.4 %, Piper multi-speaker voices 9.0 % | held-out LibriTTS-R speakers 0.93 %, **unseen Piper voices/speakers 6.73 %** |
| FA / h | 0.48 (6 in 12.43 h: CHiME-6 dev+eval 6 / 9.67 h; AudioSet eval 30-31 0 / 2.76 h) | **1.55** (21 in 13.58 h: DipCo 11 / 5.34 h; AudioSet eval 00-05 10 / 8.25 h) |
| TTS hard-negative trigger rate (any detection in the clip) | 11.2 % | 12.0 % (n = 3,520; table below) |
| General TTS speech trigger rate | n/a | 1.5 % (n = 600) |

Background evaluated: **13.58 h** of held-out test audio (DipCo 5.34 h + AudioSet evaluation
shards 00-05, 8.25 h), never trained on and never tuned on. Validation background: 12.43 h.

The test trade-off curve at W = 3, reported here and never used to choose anything: the cutoff
that gets test FA/h under 1 (0.90 → 0.88 / h) costs FRR 7.33 %. No single cutoff passes B1 and
B2 together on this model.

| cutoff | 0.70 | 0.75 | **0.80** | 0.85 | 0.90 | 0.95 | 0.97 |
|---|---|---|---|---|---|---|---|
| test FRR (noisy) | 2.80 % | 2.93 % | **3.83 %** | 5.40 % | 7.33 % | 12.23 % | 16.83 % |
| test FA / h | 2.80 | 2.36 | **1.55** | 1.10 | 0.88 | 0.22 | 0.07 |

### Hard negatives (held-out speakers/voices, noisy condition, 80 clips each)

The task's required confusables:

| phrase | "hey whisper" | "a vesper" | "vespers" | "Vespa" | "best for" | "hey vest" | "hey Esther" | bare "Vesper" |
|---|---|---|---|---|---|---|---|---|
| trigger rate | 8.8 % | 10.0 % | 1.3 % | 2.5 % | 1.3 % | 1.3 % | 11.3 % | 10.0 % |

The worst of our extra confusables: "hey Vespa" 86 %, "hey, vest pocket" 81 %, "hey vespers"
61 %, "hey, best friend" 51 %, "hey vessel" 36 %, "hey Vesta" 35 %, "hey Esper" 24 %. For "hey
Vespa" and "hey vespers", TTS output barely differs from "hey vesper" at all. Every phrase is in
`metrics.json` (`test.hardneg_per_phrase`).

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

## Iteration history

The bars and protocol were committed first (`81baf49`). Each row is one training run, scored by
`train/eval.py`, with the operating point chosen on validation.
"Bars" = B1 B2 B3.

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
| **it8** | **3.27 %** | 0.48 | 0.80 / 3 | 3.83 % (2.2 / 10.2) | 1.55 (21 / 13.58) | 12.0 % | ✓✗✓ |
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

What changed in each run (configs in `train/iterations/`):

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
- **it8**: + AudioSet bal_train 13-37 as negatives (weight 5 → 8). **Best validation FRR
  (3.27 %) → shipped** under the selection rule.
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

- **The validation FA/h underestimates the test FA/h by about 2-3×, consistently.** Every run
  was tuned to 0.40-0.48 FA/h on validation and landed at 0.66-2.06 on test. The validation
  ambient is mostly CHiME-6 dev/eval, the same corpus as the CHiME-6 *train* negatives (other
  sessions), so it's the easiest background the model sees. The test is DipCo plus AudioSet.
  The test FAs come from DipCo (dinner-party speech, 0.6-2.4 / h across runs, against
  ≤ 0.62 / h on CHiME-6 validation) and from AudioSet clips with speech, TV and music
  (0.7-2.3 / h). The protocol's margin (validation ≤ 0.5 for a bar of 1.0) wasn't big
  enough for that shift. We kept the protocol unchanged anyway.
- **The ROC frontier hardly moved.** Across the data changes, the test curves sit near
  "FRR ≈ 5 % at ≈ 1.1 FA/h, ≈ 7 % at ≈ 0.8 FA/h". The architecture (upstream mixednet, 60.8 KB;
  the size bar leaves ~4.7 KB of headroom) or the synthetic-only positives probably bound the
  result. Recall on LibriTTS-R voices is ≈ 1 % FRR. The misses are on voice models the
  training never saw (6-10 %).
- **Honesty notes.** (1) Test numbers were computed and visible for every run, as the protocol
  says, so the *direction* of some later iterations was informed by test behaviour: it10-it13
  went after the FA gap. The shipped model was still picked by the validation-only rule, which
  was written down before it9 was scored. (2) Under that rule, it8 ships. Other runs pass B2 on
  test (it2, it3, it6b), but they lost on validation (and fail B1), and picking them for that
  would be test selection. (3) `metrics.json` holds every run.

### Suggested next steps (not done here)

1. **Real audio for validation.** Record Kevin and family saying "hey vesper", plus an evening
   of the actual room/TV, through the board's mic (serial console capture, as the investigation
   doc plans). Use it as a validation set that looks like deployment. Synthetic validation is
   what misjudged FA here.
2. Re-state B2 against a bigger, more varied validation ambient (e.g. 30+ h with DipCo-like
   conversation), or accept a stricter validation target (≤ 0.3 / h).
3. A larger-capacity model within 64 KB (e.g. fewer mixconv branches, wider pointwise layers),
   or a 2-stage gate (this model + a VAD / second-stage check) to cut FAs without the recall
   cost.

## Reproduce

See `train/README.md` for the details. On the Studio, from the repo root:

```sh
bash firmware/wakeword/train/01_setup.sh
bash firmware/wakeword/train/02_download.sh
bash firmware/wakeword/train/03_generate.sh
bash firmware/wakeword/train/04_features.sh
bash firmware/wakeword/train/05_train.sh final firmware/wakeword/train/iterations/it8.yaml   # WW_SEED=21 (default)
bash firmware/wakeword/train/06_eval.sh "$HOME/builds/muse-charm/scratch/wakeword-21/runs/final/hey-vesper.tflite" /tmp/final.json final
```

Re-score the committed model at its frozen point (takes about 1 minute with the eval cache):

```sh
bash firmware/wakeword/train/06_eval.sh firmware/wakeword/hey-vesper.tflite /tmp/shipped.json shipped --fixed-cutoff 0.80 --fixed-window 3
```

It reproduces the test numbers above exactly: FRR 3.83 % / 2.20 % / 10.20 %, 21 FA in 13.58 h.

Licence: training used CC-BY-NC negative data (microWakeWord's sets) and YouTube-sourced
AudioSet, so treat this model as personal / non-commercial, the same as upstream's notebook.
