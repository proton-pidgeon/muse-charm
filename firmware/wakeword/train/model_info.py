#!/usr/bin/env python3
"""Print a .tflite's I/O contract, op list and a tensor-arena ESTIMATE (venv-mww).

nonconst_tensor_bytes = the bytes of every non-constant tensor (activations + streaming state
variables) with no memory-planner reuse. It is NOT the arena: TFLite Micro also allocates
runtime structs and per-channel quantisation data in the arena. To estimate the arena, calibrate
against a stock model with a published figure and the same op set: hey_jarvis has 9,981 B here
and 22,860 B in ESPHome's manifest, so arena ~= nonconst_tensor_bytes + ~12.9 KB.
Measure the real value on the device with MicroInterpreter::arena_used_bytes().

Usage: model_info.py <model.tflite> [more.tflite ...]
"""

import json
import sys
from collections import Counter

import numpy as np
from tensorflow.lite.tools import flatbuffer_utils


def info(path):
    m = flatbuffer_utils.read_model(path)
    sub = m.subgraphs[0]
    const_buffers = {i for i, b in enumerate(m.buffers) if b.data is not None and len(b.data) > 0}
    dt = {0: 4, 1: 2, 2: 4, 3: 1, 4: 8, 6: 1, 7: 2, 9: 1, 10: 8, 16: 4, 20: 4}
    nonconst = 0
    per_sub = []
    for sg in m.subgraphs:
        b = 0
        for t in sg.tensors:
            if t.buffer in const_buffers:
                continue
            n = int(np.prod(t.shape)) if t.shape is not None and len(t.shape) else 1
            b += n * dt.get(t.type, 4)
        per_sub.append(b)
        nonconst += b
    ops = Counter()
    for sg in m.subgraphs:
        for op in sg.operators:
            code = m.operatorCodes[op.opcodeIndex]
            ops[max(code.builtinCode, code.deprecatedBuiltinCode)] += 1
    from tensorflow.lite.python import schema_py_generated as schema

    names = {v: k for k, v in schema.BuiltinOperator.__dict__.items() if not k.startswith("_")}
    return {
        "file": path,
        "bytes": len(open(path, "rb").read()),
        "subgraphs": len(m.subgraphs),
        "ops": {names.get(k, str(k)): v for k, v in sorted(ops.items())},
        "nonconst_tensor_bytes": nonconst,
        "arena_estimate_bytes": nonconst + (22860 - 9981),
    }


if __name__ == "__main__":
    for p in sys.argv[1:]:
        print(json.dumps(info(p), indent=2))
