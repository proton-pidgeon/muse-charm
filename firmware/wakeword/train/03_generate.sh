#!/usr/bin/env bash
# Stage 3: Piper TTS synthesis of positives, hard negatives and general TTS speech for every split.
# Usage: 03_generate.sh [--scale F] [split kind ...]
#   no split/kind args = everything (train/val/test x pos/hardneg/general).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
SCALE=1.0
if [ "${1:-}" = "--scale" ]; then SCALE="$2"; shift 2; fi
if [ $# -gt 0 ]; then
  exec "$WW_PY_PIPER" -u "$WW_TRAIN_DIR/generate.py" "$1" "$2" --scale "$SCALE"
fi
for split in test val train; do
  for kind in pos hardneg general; do
    "$WW_PY_PIPER" -u "$WW_TRAIN_DIR/generate.py" "$split" "$kind" --scale "$SCALE"
  done
done
echo "generate done"
