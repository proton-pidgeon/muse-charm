#!/usr/bin/env bash
# Stage 6: score a model under the frozen protocol (../quality-bars.md).
# Usage: 06_eval.sh <model.tflite> <metrics.json> [label] [extra eval.py args...]
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
MODEL="${1:?model.tflite}"; OUT="${2:?metrics.json}"; LABEL="${3:-}"
shift $(( $# >= 3 ? 3 : $# ))
exec "$WW_PY_MWW" -u "$WW_TRAIN_DIR/eval.py" "$MODEL" --out "$OUT" --label "$LABEL" "$@"
