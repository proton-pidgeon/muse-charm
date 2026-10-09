#!/usr/bin/env python3
"""Stage 5b: export the int8 streaming tflite from a trained run (venv-mww).

Uses upstream microWakeWord's own conversion path (the one `model_train_eval` runs for
`--test_tflite_streaming_quantized 1`) without upstream's test step, because our test protocol
lives in eval.py. Loads <run>/model/best_weights.weights.h5, builds the streaming
internal-state SavedModel, and quantizes it (int8 in, uint8 out, representative data = 500
training spectrograms) to <run>/hey-vesper.tflite.

Usage: export.py <run_dir>
"""

import argparse
import os
import shutil
import sys
from pathlib import Path

import tensorflow as tf

import microwakeword.data as input_data
import microwakeword.mixednet as mixednet
import microwakeword.utils as utils
from microwakeword.layers import modes
from microwakeword.model_train_eval import load_config


def main():
    run = Path(sys.argv[1]).resolve()
    tf.keras.utils.set_random_seed(21)
    model_args = (run / "model_args.txt").read_text().split("\n")
    model_args = [a for a in model_args if a]
    assert model_args[0] == "mixednet"
    p = argparse.ArgumentParser()
    p.add_argument("--training_config")
    mixednet.model_parameters(p)
    flags = p.parse_args(["--training_config", str(run / "training_parameters.yaml")] + model_args[1:])

    config = load_config(flags, mixednet)
    data_processor = input_data.FeatureHandler(config)

    model = mixednet.model(flags, shape=config["training_input_shape"], batch_size=1)
    model.load_weights(os.path.join(config["train_dir"], "best_weights.weights.h5"))

    utils.convert_model_saved(model, config, folder="stream_state_internal",
                              mode=modes.Modes.STREAM_INTERNAL_STATE_INFERENCE)
    out_folder = os.path.join(config["train_dir"], "tflite_stream_state_internal_quant")
    utils.convert_saved_model_to_tflite(
        config,
        audio_processor=data_processor,
        path_to_model=os.path.join(config["train_dir"], "stream_state_internal"),
        folder=out_folder,
        fname="stream_state_internal_quant.tflite",
        quantize=True,
    )
    dst = run / "hey-vesper.tflite"
    shutil.copyfile(os.path.join(out_folder, "stream_state_internal_quant.tflite"), dst)
    print(f"exported {dst} ({dst.stat().st_size} bytes)", flush=True)


if __name__ == "__main__":
    main()
