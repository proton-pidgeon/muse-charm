# firmware/wakeword/train/: the "Hey Vesper" microWakeWord recipe (task 21)

This trains `../hey-vesper.tflite` on the Mac Studio from synthetic Piper TTS only. It uses no
real voice recordings and no cloud service. The only network traffic is downloads of public
artifacts: pip wheels, the pinned upstream sources, the Piper checkpoint/voices, and public
datasets from Hugging Face. It is upstream `kahrendt/microWakeWord`'s
`basic_training_notebook.ipynb`, turned into scripts and extended with a held-out test
protocol (`../quality-bars.md`).

Everything heavy (venvs, datasets, generated audio, features, checkpoints) lives **outside the
repo**, under `$WW_ROOT`, which defaults to `~/builds/muse-charm/scratch/wakeword-21`. The
footprint is about 32 GB: ~16 GB of microWakeWord's precomputed negative features, ~6 GB of
generated TTS audio, ~7 GB of our features, ~1.5 GB of extra validation ambient, and ~3 GB of
venvs. Nothing under `$WW_ROOT` is
committed.

## One command per stage

Run these from anywhere. Each stage is re-runnable and skips work that's already done.
Approximate wall time on an M4 Max is in brackets.

```sh
bash firmware/wakeword/train/01_setup.sh              # pinned sources + 2 uv venvs + Piper checkpoint   [3 min]
bash firmware/wakeword/train/02_download.sh           # RIRs, noise, negative features, held-out background + extra validation ambient [30-50 min, ~27 GB]
bash firmware/wakeword/train/03_generate.sh           # Piper TTS: positives, hard negatives, general speech  [~2.5 h; lt on MPS, onnx voices on CPU]
bash firmware/wakeword/train/04_features.sh           # augment + spectrogram RaggedMmaps (+ AudioSet bal_train 03-37 negatives) [~45 min]
bash firmware/wakeword/train/04b_mine.sh scan|build   # hard-negative mining over the TRAINING negatives (needs trained models; see below) [~1 min per scan]
bash firmware/wakeword/train/05_train.sh final        # train (seeded, = iterations/it14.yaml, WW_SEED 22) + export int8 streaming tflite [~8 min, CPU]
bash firmware/wakeword/train/06_eval.sh "$HOME/builds/muse-charm/scratch/wakeword-21/runs/final/hey-vesper.tflite" /tmp/metrics.json final
```

`training_parameters.yaml` is `iterations/it14.yaml`, the shipped run (trained with
`WW_SEED=22`, the default since it14), and the default stages build exactly its data, except
the mined set, which is built from earlier models:

```sh
# the shipped mined set r1 = the union of 7 earlier models' scans (it8 it1 it5 it9 it9b it12s22 it11s22).
# Train those first with their configs (WW_SEED=21; it11s22/it12s22 with WW_SEED=22 and it10's data, see below), then:
for r in it8 it1 it5 it9 it9b it12s22 it11s22; do
  bash firmware/wakeword/train/04b_mine.sh scan "$WW_ROOT/runs/$r/hey-vesper.tflite" "$WW_ROOT/runs/mine_$r.npz" --min-prob 0.3
done
bash firmware/wakeword/train/04b_mine.sh build r1 "$WW_ROOT"/runs/mine_{it8,it1,it5,it9,it9b,it12s22,it11s22}.npz --min-prob 0.5
```

That writes `$WW_FEAT/mined_neg_r1/training/r1_mmap` (6,814 windows for the committed scans;
a rebuild of the seven models gives a statistically equivalent set, not the same windows).
Mining reads training negatives only; see `mine_negatives.py` (`SOURCES`) and
`../quality-bars.md` addendum #2.

To re-run another iteration, pass its config
(`05_train.sh it12 firmware/wakeword/train/iterations/it12.yaml`) and seed (`WW_SEED=21` for
it1-it13 and it14's first seed). `WW_INIT_FROM=<run>` (it18, it19) starts from that run's
final weights (a fine-tune). Iterations it10-it13 also had two opt-in data additions:
`04_features.sh audioset_unbal`, and CHiME-6 array u02, kept via
`rm -rf $WW_ROOT/data/negative_datasets/dinner_party && WW_KEEP_CHIME_U02=1 02_download.sh negatives`.
Earlier iterations used less TTS data (see the COUNTS comments in `generate.py` and
`../README.md`). Those exact data states are documented, not re-buildable by flag.

To re-check the shipped model at its frozen operating point (no re-selection):

```sh
bash firmware/wakeword/train/06_eval.sh firmware/wakeword/hey-vesper.tflite /tmp/m.json shipped --fixed-cutoff <c> --fixed-window <W>
```

`06_eval.sh` needs the eval feature cache that the first full run builds (`$WW_FEAT/eval/`).

## Files

| File | Stage | What |
|---|---|---|
| `env.sh` | all | paths (`WW_ROOT` etc.), `PYTHONHASHSEED=0`; sourced by every stage |
| `01_setup.sh` | 1 | microWakeWord @ `4665173` and piper-sample-generator @ `2971426` (tarballs), `venv-mww` (TF 2.20 CPU) and `venv-piper` (torch 2.11, MPS) from the lock files, the LibriTTS-R generator `.pt` (sha256 checked by eye in the log) |
| `requirements-*.in` / `.lock` | 1 | top-level pins / fully resolved `uv pip compile` locks (Python 3.11, macOS arm64) |
| `02_download.sh` → `download_data.py` | 2 | MIT RIRs (split ~80/20 by name hash into train/test pools), AudioSet bal_train 00-01 + FMA-xs (train noise), bal_train 02 (test noise), microWakeWord negative features (3 redundant mic copies pruned for disk), AudioSet *evaluation* shards 30-31 + 06-15 (val ambient; 06-15 = `background_val_ext`, addendum #2) and 00-05 (test background) |
| `03_generate.sh` → `generate.py`, `phrases.py` | 3 | speaker-disjoint train/val/test TTS synthesis (LibriTTS-R generator + stock Piper voices); per-split synthesis settings; per-batch seeds |
| `04_features.sh` → `features.py`, `augment_cfg.py` | 4 | augmentation (train pools only) + TFLM micro-frontend features as uint16 RaggedMmaps; AudioSet bal_train 03-12 as negatives |
| `04b_mine.sh` → `mine_negatives.py` | 4b | hard-negative mining: stream a trained model over the TRAINING negative sets, keep the windows where it fires (≥ 0.5 after a ≥ 0.3 scan), store them end-aligned as `$WW_FEAT/mined_neg_<tag>/` (it14+) |
| `05_train.sh` → `seeded_train.py`, `export.py`, `training_parameters.yaml` | 5 | upstream `microwakeword.model_train_eval` (mixednet, stride 3) with fixed seeds; export via upstream's converter to the int8-in/uint8-out streaming tflite |
| `06_eval.sh` → `eval.py` | 6 | the frozen protocol: validation-only operating point, test FRR/FA/h, hard-negative table |
| `model_info.py` | - | op list + tensor-arena estimate for a tflite |
| `iterations/itN.yaml` | - | the training config of each iteration (`it14.yaml` = `training_parameters.yaml`, the shipped run at `WW_SEED=22`; `it11`-`it14` were run with several `WW_SEED`s; `it18`/`it19` are fine-tunes, `WW_INIT_FROM=it8`) |

## Confirmatory holdout (addendum #3)

Scores the committed model once, at the frozen point, on AudioSet evaluation shards 16-29 (14 x
~700 MB downloaded one at a time and deleted; ~19.2 h). Needs ~3 GB free beyond the cache:

```
./02_download.sh background_confirm && ./06_eval.sh ../hey-vesper.tflite confirm_metrics.json confirm --fixed-cutoff 0.65 --fixed-window 3 --confirm-only
```

`WW_CONFIRM_SHARDS=16,17` limits the shards. The feature cache (`$WW_FEAT/eval/ambient/`) makes a
rerun instant, and the raw wavs are not needed after the first run.

## Determinism

Seeds are fixed for: the speaker splits, voice choice and synthesis settings per clip
(`generate.py`, seed 21), torch synthesis noise for the LibriTTS-R generator (per-batch
`torch.manual_seed`), augmentation (`random`/`numpy` seeded per feature set), batch sampling and
weight init (`seeded_train.py`: `tf.keras.utils.set_random_seed`, `WW_SEED`, default 22 = the
shipped run; it1-it13 and it14's first run used 21), and the
eval-set augmentation (cached in `$WW_FEAT/eval/` on the first eval run). Three sources stay
non-deterministic: the onnxruntime noise inside stock Piper voices (so a few `xl` clips land on
either side of the 2.6 s length filter on a rerun), MPS float kernels, and the order of TF CPU
float reductions. A rerun gives a statistically equivalent model, not a byte-identical one. Seed
to seed, the validation FRR of one config moved over 5.2-7.3 % (`it11`) and 2.5-5.1 % (`it14`),
so treat single-run differences under ~2 points as noise, and expect a rerun to need a second
seed to land in the shipped run's band. The committed `hey-vesper.tflite` + `metrics.json` are
the reference artifacts.

## Licences of what gets downloaded

microWakeWord, piper-sample-generator and Piper are Apache-2.0/MIT. The training data has mixed
licences: microWakeWord's negative sets are CC-BY-NC-4.0, AudioSet clips are YouTube-sourced,
and FMA is CC. Upstream's notebook therefore calls any model trained this way **personal /
non-commercial use**, and that applies here too.
