#!/usr/bin/env bash
# Stage 2: download public datasets (RIRs, noise, negative features, held-out background).
# Usage: 02_download.sh [rirs] [noise] [negatives] [background]   (default: all)
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
exec "$WW_PY_MWW" -u "$WW_TRAIN_DIR/download_data.py" "$@"
