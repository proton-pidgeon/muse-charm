#!/usr/bin/env bash
# Stage 1: pinned upstream sources + two uv venvs + the Piper LibriTTS-R generator checkpoint.
#   venv-mww   : Python 3.11, TensorFlow (CPU) + microWakeWord (training, features, export, eval)
#   venv-piper : Python 3.11, torch (MPS) + piper-sample-generator (TTS sample generation)
# Re-runnable: skips what already exists. Installs from the pinned lock files in this directory.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

MWW_REPO="kahrendt/microWakeWord"
MWW_SHA="4665173cd35f1cff9a61e06fc427f124766c488e"        # HEAD 2026-07-06
PSG_REPO="rhasspy/piper-sample-generator"
PSG_SHA="2971426a55072f7d22fec416ca7800df8bd23207"        # HEAD 2026-03-12 (v3.2.0, MPS-capable)
GH="https://codeload.github.com"
PIPER_PT_URL="https://github.com/rhasspy/piper-sample-generator/releases/download/v2.0.0/en_US-libritts_r-medium.pt"

fetch_src() { # repo sha dest
  if [ -d "$3" ]; then echo "src ok: $3"; return; fi
  mkdir -p "$3"
  curl -fsSL "$GH/$1/tar.gz/$2" | tar -xz -C "$3" --strip-components=1
  echo "$1@$2" > "$3/.pinned"
}
fetch_src "$MWW_REPO" "$MWW_SHA" "$WW_SRC/microWakeWord"
fetch_src "$PSG_REPO" "$PSG_SHA" "$WW_SRC/piper-sample-generator"

PT="$WW_SRC/piper-sample-generator/models/en_US-libritts_r-medium.pt"
if [ ! -s "$PT" ]; then curl -fsSL -o "$PT" "$PIPER_PT_URL"; fi
shasum -a 256 "$PT"   # e95ee53770bf598c354a6e6dbfc95ccb259aeeb501d35a86be8a767429ab0ff6

site_dir() { "$1" -c 'import sysconfig; print(sysconfig.get_paths()["purelib"])'; }

if [ ! -x "$WW_PY_MWW" ]; then uv venv -q --python 3.11 "$WW_ROOT/venv-mww"; fi
VIRTUAL_ENV="$WW_ROOT/venv-mww" uv pip install -q -r "$WW_TRAIN_DIR/requirements-mww.lock"

if [ ! -x "$WW_PY_PIPER" ]; then uv venv -q --python 3.11 "$WW_ROOT/venv-piper"; fi
VIRTUAL_ENV="$WW_ROOT/venv-piper" uv pip install -q -r "$WW_TRAIN_DIR/requirements-piper.lock"

# Neither upstream is pip-installed. microWakeWord's setup.py misses its `audio`/`layers`
# subpackages (the notebook uses `pip install -e`), and the Piper generator checkpoint unpickles
# classes from the repo's un-packaged `piper_train`. A .pth file puts each source tree on sys.path.
echo "$WW_SRC/microWakeWord" > "$(site_dir "$WW_PY_MWW")/ww21_microwakeword.pth"
echo "$WW_SRC/piper-sample-generator" > "$(site_dir "$WW_PY_PIPER")/ww21_piper_sample_generator.pth"

"$WW_PY_MWW" -c "import tensorflow as tf, microwakeword.audio.augmentation, pymicro_features, ai_edge_litert; print('mww ok, tf', tf.__version__)"
"$WW_PY_PIPER" -c "import torch, piper, piper_sample_generator.__main__, piper_train; print('piper ok, torch', torch.__version__, 'mps', torch.backends.mps.is_available())"
