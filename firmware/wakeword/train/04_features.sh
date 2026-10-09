#!/usr/bin/env bash
# Stage 4: augment generated clips -> uint16 spectrogram RaggedMmaps (train + val splits).
# Usage: 04_features.sh [positives] [hardneg] [audioset_neg] [val_ambient]   (default: all)
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
exec "$WW_PY_MWW" -u "$WW_TRAIN_DIR/features.py" "$@"
