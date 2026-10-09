#!/usr/bin/env python3
"""Task 22: does the firmware's "Hey Vesper" pipeline match what the model was scored with?

Runs in the task-21 venv (venv-mww: pymicro_features, ai_edge_litert, train/eval.py's helpers).

  check.py <vwe_host> <model.tflite> [--pos N] [--neg N] [--stream-min M]

For a sample of task 21's TEST clips (positives, general speech) padded with 1.0 s of silence
before and 0.5 s after, and M minutes of a background recording:

  firmware side  vwe_host = hatch/vesper_wakeword_engine.cc + hatch/vesper_mww.c on TFLite Micro
                 (reference kernels) and the microfrontend the firmware links; 20 ms chunks
                 (10 ms for the stream, so no audio is skipped after a detection)
  training side  train/eval.py: pymicro_features -> uint16 -> quantize_input -> the LiteRT
                 interpreter, one fresh interpreter per clip -> count_detections(W 3, 0.65)

and compares the int8 model inputs (must be identical: the feature pipeline is the training
one), the detections (must agree with eval.py's), and the uint8 outputs (reported: TFLite Micro's
integer kernels round a little differently from LiteRT's, against both its reference kernels and
the XNNPACK ones eval.py scored with). Exit 0 only if inputs and every verdict agree.
"""

import argparse
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import librosa
import numpy as np
from scipy.io import wavfile

ROOT = Path(os.environ.get("WW_ROOT", Path.home() / "builds/muse-charm/scratch/wakeword-21"))
os.environ.setdefault("WW_FEAT", str(ROOT / "features"))
os.environ.setdefault("WW_GEN", str(ROOT / "gen"))
os.environ.setdefault("WW_DATA", str(ROOT / "data"))
TRAIN = Path(__file__).resolve().parent.parent / "train"
sys.path.insert(0, str(TRAIN))
spec = importlib.util.spec_from_file_location("ww_eval", TRAIN / "eval.py")
ww = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ww)

CUTOFF, WINDOW = 0.65, 3


def stream_ref_kernels(q):
    """eval.py's stream() on LiteRT's reference kernels (BUILTIN_REF) instead of its default
    (XNNPACK): TFLite Micro's kernels are the reference ones, so these should agree exactly."""
    from ai_edge_litert.interpreter import Interpreter, OpResolverType

    it = Interpreter(model_content=MODEL, experimental_op_resolver_type=OpResolverType.BUILTIN_REF)
    it.allocate_tensors()
    inp, outp = it.get_input_details()[0], it.get_output_details()[0]
    out = np.empty(len(q), dtype=np.uint8)
    for i, x in enumerate(q):
        it.set_tensor(inp["index"], x.reshape(inp["shape"]))
        it.invoke()
        out[i] = it.get_tensor(outp["index"]).reshape(-1)[0]
    return out


def reference(pcm):
    """eval.py's path for one track: (int8 inputs [n,3,40], uint8 outputs [n] as eval.py scored
    them, uint8 outputs [n] on reference kernels)."""
    f = ww.features_of(pcm)
    q = ww.quantize_input(f, *SCALE_ZP)
    n = q.shape[0] // ww.STRIDE
    q = q[: n * ww.STRIDE].reshape(n, ww.STRIDE, 40)
    return q, ww.stream(f), stream_ref_kernels(q)


def run_host(host, model, wavs, chunk, dump):
    out = subprocess.run([host, "--chunk", str(chunk), "--cutoff", str(round(CUTOFF * 1000)), "--window",
                          str(WINDOW), "--dump", dump, model, *map(str, wavs)],
                         check=True, capture_output=True, text=True).stdout
    lines = [json.loads(x) for x in out.splitlines()]
    return lines[:-1], lines[-1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("model")
    ap.add_argument("--pos", type=int, default=60)
    ap.add_argument("--neg", type=int, default=60)
    ap.add_argument("--stream-min", type=float, default=10.0)
    a = ap.parse_args()

    ww._init_worker(a.model)
    global MODEL
    MODEL = Path(a.model).read_bytes()
    from ai_edge_litert.interpreter import Interpreter

    it = Interpreter(model_content=Path(a.model).read_bytes())
    global SCALE_ZP
    SCALE_ZP = it.get_input_details()[0]["quantization"]

    rng = np.random.default_rng(22)
    def pick(d, n):
        files = sorted((ROOT / "gen" / d).glob("**/*.wav"))
        return [files[i] for i in sorted(rng.choice(len(files), size=min(n, len(files)), replace=False))]

    sets = {"positive": pick("pos/test", a.pos), "general_speech": pick("general/test", a.neg)}
    bg = sorted((ROOT / "data" / "background_val_ext").glob("*.wav"))
    tmp = Path(tempfile.mkdtemp(prefix="vwe-check-"))
    jobs = []   # (kind, name, pcm, wav path)
    for kind, files in sets.items():
        for f in files:
            # As eval.py loads clips (librosa, resampled to 16 kHz), then microWakeWord's int16 step.
            audio, _ = librosa.load(str(f), sr=16000, mono=True)
            pcm = np.clip(audio * 32768, -32768, 32767).astype(np.int16)
            pcm = np.concatenate([np.zeros(16000, np.int16), pcm, np.zeros(8000, np.int16)])
            p = tmp / f"{len(jobs)}.wav"
            wavfile.write(str(p), 16000, pcm)
            jobs.append((kind, f"{f.parent.name}/{f.name}", pcm, p))
    stream = None
    if bg and a.stream_min > 0:
        sr, pcm = wavfile.read(str(bg[0]))
        pcm = pcm[: int(a.stream_min * 60 * 16000)]
        p = tmp / "stream.wav"
        wavfile.write(str(p), 16000, pcm)
        stream = (bg[0].name, pcm, p)

    ok = True
    report = {"clips": {}, "input_mismatches": 0, "verdict_mismatches": 0}
    dump = tmp / "clips"
    dump.mkdir()
    rows, summary = run_host(a.host, a.model, [j[3] for j in jobs], 320, str(dump))
    diffs = []
    stats = {"n": 0, "same_as_ref_kernels": 0, "same_as_eval": 0, "max_diff_ref_kernels": 0}
    for k, ((kind, name, pcm, _), row) in enumerate(zip(jobs, rows)):
        ref_in, ref_p, ref_k = reference(pcm)
        host_in = np.fromfile(dump / f"{k}.in", np.int8).reshape(-1, 3, 40)
        host_p = np.fromfile(dump / f"{k}.p", np.uint8)
        n = min(len(host_in), len(ref_in))
        # A detection drops the rest of its 20 ms chunk (as in the firmware, which then listens
        # and resets), so the clip is comparable up to its first detection.
        n = min(n, len(host_p))
        if row["first_detection"] >= 0:
            n = min(n, row["first_detection"] + 1)
        if not np.array_equal(host_in[:n], ref_in[:n]):
            report["input_mismatches"] += 1
            ok = False
        d = int(np.max(np.abs(host_p[:n].astype(int) - ref_p[:n].astype(int)))) if n else 0
        diffs.append(d)
        stats["n"] += n
        stats["same_as_ref_kernels"] += int(np.sum(host_p[:n] == ref_k[:n]))
        stats["same_as_eval"] += int(np.sum(host_p[:n] == ref_p[:n]))
        stats["max_diff_ref_kernels"] = max(stats["max_diff_ref_kernels"],
                                            int(np.max(np.abs(host_p[:n].astype(int) - ref_k[:n].astype(int)))) if n else 0)
        ref_trig = ww.clip_triggers(ref_p, WINDOW, CUTOFF)
        host_trig = row["detections"] > 0
        if ref_trig != host_trig:
            report["verdict_mismatches"] += 1
            ok = False
        c = report["clips"].setdefault(kind, {"n": 0, "host_detected": 0, "reference_detected": 0})
        c["n"] += 1
        c["host_detected"] += host_trig
        c["reference_detected"] += ref_trig
    report["clip_outputs"] = {
        "inferences": stats["n"],
        "identical_to_litert_reference_kernels": stats["same_as_ref_kernels"],
        "max_diff_to_litert_reference_kernels": stats["max_diff_ref_kernels"],
        "identical_to_eval_py_xnnpack": stats["same_as_eval"],
        "max_diff_to_eval_py_xnnpack": max(diffs) if diffs else 0,
    }
    report["arena_used_host"] = summary["arena_used"]
    report["host_us_per_inference"] = summary["us_per_feed_with_inference"]

    if stream:
        name, pcm, p = stream
        sdump = tmp / "stream"
        sdump.mkdir()
        srows, _ = run_host(a.host, a.model, [p], 160, str(sdump))
        ref_in, ref_p, ref_k = reference(pcm)
        host_in = np.fromfile(sdump / "0.in", np.int8).reshape(-1, 3, 40)
        host_p = np.fromfile(sdump / "0.p", np.uint8)
        n = min(len(host_in), len(ref_in))
        same = bool(np.array_equal(host_in[:n], ref_in[:n]))
        ok &= same
        ref_det = ww.count_detections(ref_p, WINDOW, CUTOFF)
        host_det = srows[0]["detections"]
        ok &= ref_det == host_det
        report["stream"] = {
            "source": name, "minutes": round(len(pcm) / 16000 / 60, 2), "inferences": n, "inputs_identical": same,
            "outputs_identical_to_litert_reference_kernels": int(np.sum(host_p[:n] == ref_k[:n])),
            "max_diff_to_litert_reference_kernels": int(np.max(np.abs(host_p[:n].astype(int) - ref_k[:n].astype(int)))),
            "outputs_identical_to_eval_py_xnnpack": int(np.sum(host_p[:n] == ref_p[:n])),
            "max_diff_to_eval_py_xnnpack": int(np.max(np.abs(host_p[:n].astype(int) - ref_p[:n].astype(int)))),
            "host_detections": host_det, "reference_detections": int(ref_det),
        }
    report["pass"] = bool(ok)
    print(json.dumps(report, indent=2))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
