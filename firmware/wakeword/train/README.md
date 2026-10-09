# firmware/wakeword/train/: the "Hey Vesper" microWakeWord recipe (task 21)

This trains `../hey-vesper.tflite` on the Mac Studio from synthetic Piper TTS only. It uses no
real voice recordings and no cloud service. The only network traffic is downloads of public
artifacts: pip wheels, the pinned upstream sources, the Piper checkpoint/voices, and public
datasets from Hugging Face. It is upstream `kahrendt/microWakeWord`'s
`basic_training_notebook.ipynb`, turned into scripts and extended with a held-out test
protocol (`../quality-bars.md`).

Everything heavy (venvs, datasets, generated audio, features, checkpoints) lives **outside the
repo**, under `$WW_ROOT`, which defaults to `~/builds/muse-charm/scratch/wakeword-21`. The
footprint is about 27 GB, mostly microWakeWord's precomputed negative features. Nothing under
`$WW_ROOT` is committed.

## One command per stage

Run these from anywhere. Each stage is re-runnable and skips work that's already done.
Approximate wall time on an M4 Max is in brackets.

```sh
bash firmware/wakeword/train/01_setup.sh              # pinned sources + 2 uv venvs + Piper checkpoint   [3 min]
bash firmware/wakeword/train/02_download.sh           # RIRs, noise, negative features, held-out background [20-40 min, ~25 GB]
bash firmware/wakeword/train/03_generate.sh           # Piper TTS: positives, hard negatives, general speech  [~40 min, MPS]
bash firmware/wakeword/train/04_features.sh           # augment + spectrogram RaggedMmaps (+ AudioSet bal_train negatives) [~15 min]
bash firmware/wakeword/train/05_train.sh final        # train (seeded) + export int8 streaming tflite        [~15 min, CPU]
bash firmware/wakeword/train/06_eval.sh "$HOME/builds/muse-charm/scratch/wakeword-21/runs/final/hey-vesper.tflite" /tmp/metrics.json final
```

To reproduce an earlier iteration, pass its config: `05_train.sh it2 firmware/wakeword/train/iterations/it2.yaml`.
The data-side changes between iterations (counts, repeats, stored slides) are in the README's
iteration history. The committed scripts produce the **final** iteration's data.

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
| `02_download.sh` → `download_data.py` | 2 | MIT RIRs (split ~80/20 by name hash into train/test pools), AudioSet bal_train 00-01 + FMA-xs (train noise), bal_train 02 (test noise), microWakeWord negative features (3 redundant mic copies pruned for disk), AudioSet *evaluation* shards 30-31 (val ambient) and 00-05 (test background) |
| `03_generate.sh` → `generate.py`, `phrases.py` | 3 | speaker-disjoint train/val/test TTS synthesis (LibriTTS-R generator + stock Piper voices); per-split synthesis settings; per-batch seeds |
| `04_features.sh` → `features.py`, `augment_cfg.py` | 4 | augmentation (train pools only) + TFLM micro-frontend features as uint16 RaggedMmaps; AudioSet bal_train 03-12 as negatives |
| `05_train.sh` → `seeded_train.py`, `export.py`, `training_parameters.yaml` | 5 | upstream `microwakeword.model_train_eval` (mixednet, stride 3) with fixed seeds; export via upstream's converter to the int8-in/uint8-out streaming tflite |
| `06_eval.sh` → `eval.py` | 6 | the frozen protocol: validation-only operating point, test FRR/FA/h, hard-negative table |
| `model_info.py` | - | op list + tensor-arena estimate for a tflite |
| `iterations/itN.yaml` | - | the training config of each iteration (the final one equals `training_parameters.yaml`) |

## Determinism

Seeds are fixed for: the speaker splits and per-batch TTS settings (`generate.py`, seed 21),
torch synthesis noise (per-batch `torch.manual_seed`), augmentation (`random`/`numpy` seeded per
feature set), batch sampling and weight init (`seeded_train.py`: `tf.keras.utils.set_random_seed(21)`),
and the eval-set augmentation. Two sources stay non-deterministic: MPS float kernels in TTS, and
the order of CPU float reductions in TF. A rerun gives a statistically equivalent model, not a
byte-identical one. The committed `hey-vesper.tflite` + `metrics.json` are the reference
artifacts.

## Licences of what gets downloaded

microWakeWord, piper-sample-generator and Piper are Apache-2.0/MIT. The training data has mixed
licences: microWakeWord's negative sets are CC-BY-NC-4.0, AudioSet clips are YouTube-sourced,
and FMA is CC. Upstream's notebook therefore calls any model trained this way **personal /
non-commercial use**, and that applies here too.
