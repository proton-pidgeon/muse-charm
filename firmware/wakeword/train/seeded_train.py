#!/usr/bin/env python3
"""Run upstream `python -m microwakeword.model_train_eval ...` with fixed seeds.

Seeds Python `random`, NumPy and TensorFlow (tf.keras.utils.set_random_seed) before handing
over to the unmodified upstream entry point. That pins the batch sampling (data.py uses
random/np.random) and the weight init. CPU float reductions can still differ in the last
bits between machines.
"""

import runpy
import sys

import tensorflow as tf

SEED = 21

tf.keras.utils.set_random_seed(SEED)  # seeds random, numpy and tf
sys.argv = ["microwakeword.model_train_eval"] + sys.argv[1:]
runpy.run_module("microwakeword.model_train_eval", run_name="__main__", alter_sys=True)
