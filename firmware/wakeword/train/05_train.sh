#!/usr/bin/env bash
# Stage 5: train the mixednet model (upstream microwakeword.model_train_eval, CPU TensorFlow),
# then export the int8 streaming tflite (export.py).
# Usage: [WW_SEED=21] [WW_INIT_FROM=<run>] 05_train.sh <run-name> [training_parameters.yaml]
# Output: $WW_RUNS/<run>/model/ (checkpoints, best_weights) and $WW_RUNS/<run>/hey-vesper.tflite
# WW_INIT_FROM=<run> (it18+): start from that run's final weights + optimiser state (its
# model/restore/ checkpoint is copied in; upstream restores it and counts steps from 1 again),
# i.e. a fine-tune. The yaml's training_steps / learning_rates are then the fine-tune schedule.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
RUN="${1:?run name}"
YAML="${2:-$WW_TRAIN_DIR/training_parameters.yaml}"
export RUN
mkdir -p "$WW_RUNS/$RUN"
if [ -n "${WW_INIT_FROM:-}" ] && [ ! -d "$WW_RUNS/$RUN/model/restore" ]; then
  mkdir -p "$WW_RUNS/$RUN/model"
  cp -R "$WW_RUNS/$WW_INIT_FROM/model/restore" "$WW_RUNS/$RUN/model/restore"
  echo "$WW_INIT_FROM" > "$WW_RUNS/$RUN/init_from.txt"
fi
"$WW_PY_MWW" -c 'import os,sys; t=open(sys.argv[1]).read(); open(sys.argv[2],"w").write(os.path.expandvars(t))' \
  "$YAML" "$WW_RUNS/$RUN/training_parameters.yaml"

# The upstream notebook's architecture (stride 3, ~60 KB int8). Keep in sync with export.py.
MODEL_ARGS=(mixednet
  --pointwise_filters "64,64,64,64"
  --repeat_in_block "1,1,1,1"
  --mixconv_kernel_sizes "[5],[7,11],[9,15],[23]"
  --residual_connection "0,0,0,0"
  --first_conv_filters 32
  --first_conv_kernel_size 5
  --stride 3)
printf '%s\n' "${MODEL_ARGS[@]}" > "$WW_RUNS/$RUN/model_args.txt"

cd "$WW_RUNS/$RUN"
"$WW_PY_MWW" -u "$WW_TRAIN_DIR/seeded_train.py" \
  --training_config "$WW_RUNS/$RUN/training_parameters.yaml" \
  --train 1 --restore_checkpoint 1 \
  --test_tf_nonstreaming 0 --test_tflite_nonstreaming 0 --test_tflite_nonstreaming_quantized 0 \
  --test_tflite_streaming 0 --test_tflite_streaming_quantized 0 \
  "${MODEL_ARGS[@]}"

"$WW_PY_MWW" -u "$WW_TRAIN_DIR/export.py" "$WW_RUNS/$RUN"
