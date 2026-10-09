#!/usr/bin/env bash
# Stage 4b (it14+): hard-negative mining over the TRAINING negative feature sets (mine_negatives.py).
# Usage: 04b_mine.sh scan  <model.tflite> <scan.npz> [--min-prob 0.3]
#        04b_mine.sh build <tag> <scan.npz> [more.npz ...] [--min-prob 0.5]   -> $WW_FEAT/mined_neg_<tag>/
#        04b_mine.sh stats <scan.npz>
# The shipped model (it14, seed 22) uses tag r1 = the union of 7 earlier models' scans; see README.md.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
exec "$WW_PY_MWW" -u "$WW_TRAIN_DIR/mine_negatives.py" "$@"
