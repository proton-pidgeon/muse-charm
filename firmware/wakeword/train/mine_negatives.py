#!/usr/bin/env python3
"""Stage 4b (it14+): hard-negative mining over the TRAINING negative feature sets.

Runs a trained streaming model (the same int8 .tflite the device runs, same decision rule as
eval.py) over every training negative spectrogram (microWakeWord's speech / dinner_party /
no_speech sets, our AudioSet bal_train + train-noise-pool negatives) and records every place
where the model fires or nearly fires. Those windows are then stored as a dedicated,
end-aligned negative feature set (`$WW_FEAT/mined_neg/training/<tag>_mmap`, 224 frames each,
the detection at the window end), which the next training run samples with its own weight and
`fixed_right_cutoff` 0-9, exactly like the aligned TTS hard negatives.

Only TRAINING data is read. Nothing from the validation ambient, the test background, the test
RIR/noise pools or the held-out TTS splits is ever touched here (see the SOURCES list).

Usage:
  mine_negatives.py scan   <model.tflite> <scan.npz> [--min-prob 0.3] [--workers N]
  mine_negatives.py build  <scan.npz> <tag> [--min-prob 0.5] [--max-per-source N]
  mine_negatives.py stats  <scan.npz>

`scan` is model-specific and slow-ish (~5-10 min for ~530 h on 14 workers); `build` is instant
and can be re-run with another threshold. `tag` names the output mmap.
"""

import argparse
import os
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

FEAT = Path(os.environ["WW_FEAT"])
DATA = Path(os.environ["WW_DATA"])

STRIDE = 3
WINDOW = 3            # the firmware's W for mining; eval.py may still pick another W
COOLDOWN = 25         # as eval.py: ignore 25 inferences after a hit and at the start of a chunk
KEEP_FRAMES = 224     # as features.py KEEP_FRAMES_POS: 204 model frames + 0-9 cutoff + margin
END_MARGIN = 5        # frames stored after the detection end (so cutoff 0..9 brackets the hit)
TASK_CHUNKS = 2000    # spectrograms per worker task

# Training-only negative sources (name -> RaggedMmap dir). Nothing else is ever scanned.
SOURCES = {
    "voices_far": DATA / "negative_datasets/speech/training/voices_lav_far_training_mmap",
    "chime6_train": DATA / "negative_datasets/dinner_party/training/chime6_train_u01_ch1_mmap",
    "fma_medium": DATA / "negative_datasets/no_speech/training/fma_medium_mmap",
    "fsd50k_no_speech": DATA / "negative_datasets/no_speech/training/fsd50k_no_speech_mmap",
    "fsd50k_speech": DATA / "negative_datasets/no_speech/training/fsd50k_speech_mmap",
    "wham": DATA / "negative_datasets/no_speech/training/wham_train_mmap",
    "noise_train_pool": FEAT / "audioset_neg/training/noise_train_pool_mmap",
    **{f"audioset_bal_{s:02d}": FEAT / f"audioset_neg/training/audioset_bal_train_{s:02d}_mmap"
       for s in range(3, 38)},
}


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


# ------------------------------------------------------------------------------------------
# streaming inference in workers (same quantisation + rule as eval.py)

_MODEL = None
_IT = None


def _init_worker(model_path):
    global _MODEL
    _MODEL = Path(model_path).read_bytes()


def _interp():
    global _IT
    if _IT is None:
        from ai_edge_litert.interpreter import Interpreter

        it = Interpreter(model_content=_MODEL)
        it.allocate_tensors()
        _IT = (it, it.get_input_details()[0], it.get_output_details()[0])
    return _IT


def quantize_input(feats_u16, scale, zp):
    if abs(scale - 0.1015625) < 1e-9 and zp == -128:
        q = (feats_u16.astype(np.int64) * 256 + 333) // 666 - 128
    else:
        q = np.round(feats_u16.astype(np.float64) / 25.6 / scale) + zp
    return np.clip(q, -128, 127).astype(np.int8)


def stream(feats_u16):
    it, inp, outp = _interp()
    it.reset_all_variables()  # fresh streaming state per chunk, like a boot
    scale, zp = inp["quantization"]
    q = quantize_input(feats_u16, scale, zp)
    n = q.shape[0] // STRIDE
    out = np.empty(n, dtype=np.uint8)
    for i in range(n):
        it.set_tensor(inp["index"], q[i * STRIDE:(i + 1) * STRIDE].reshape(inp["shape"]))
        it.invoke()
        out[i] = it.get_tensor(outp["index"]).reshape(-1)[0]
    return out


def moving_mean(p, w):
    c = np.cumsum(np.concatenate([np.zeros(w, dtype=np.int64), p.astype(np.int64)]))
    return (c[w:] - c[:-w]) / (255.0 * w)


def peaks(p, min_prob):
    """[(inference index, peak prob)] of separate hits (>= min_prob), with eval.py's cooldown.
    The recorded index is the max of each hit's run, the recorded prob is that max."""
    m = moving_mean(p, WINDOW)
    hits = []
    i, n = COOLDOWN, len(m)
    while i < n:
        if m[i] >= min_prob:
            j_end = min(n, i + COOLDOWN + 1)
            j = i + int(np.argmax(m[i:j_end]))
            hits.append((int(j), float(m[j])))
            i = i + COOLDOWN + 1
        else:
            i += 1
    return hits


def scan_task(args):
    src, path, start, stop, min_prob = args
    from mmap_ninja.ragged import RaggedMmap

    r = RaggedMmap(str(path))
    rows = []
    for idx in range(start, stop):
        feats = np.asarray(r[idx])
        if feats.shape[0] < STRIDE * (COOLDOWN + 5):
            continue
        for infer_idx, prob in peaks(stream(feats), min_prob):
            rows.append((src, idx, (infer_idx + 1) * STRIDE, prob))  # end frame of the hit
    return rows


def scan(a):
    model = Path(a.model).resolve()
    from mmap_ninja.ragged import RaggedMmap

    tasks = []
    total = 0
    for src, path in SOURCES.items():
        if not path.exists():
            raise SystemExit(f"missing source {src}: {path}")
        n = len(RaggedMmap(str(path)))
        total += n
        for s in range(0, n, TASK_CHUNKS):
            tasks.append((src, path, s, min(n, s + TASK_CHUNKS), a.min_prob))
    log(f"scanning {total} spectrograms from {len(SOURCES)} training sources in {len(tasks)} tasks")
    rows = []
    t0 = time.time()
    with ProcessPoolExecutor(max_workers=a.workers, initializer=_init_worker, initargs=(str(model),)) as ex:
        for k, r in enumerate(ex.map(scan_task, tasks)):
            rows.extend(r)
            if k % 20 == 0:
                log(f"task {k}/{len(tasks)}, hits so far {len(rows)}, {time.time() - t0:.0f}s")
    src = np.array([r[0] for r in rows])
    idx = np.array([r[1] for r in rows], dtype=np.int64)
    end = np.array([r[2] for r in rows], dtype=np.int64)
    prob = np.array([r[3] for r in rows], dtype=np.float32)
    np.savez(a.out, src=src, idx=idx, end=end, prob=prob, model=str(model), min_prob=a.min_prob)
    log(f"saved {len(rows)} candidate hits to {a.out} in {time.time() - t0:.0f}s")
    stats_of(src, prob)


def stats_of(src, prob):
    for thr in (0.3, 0.5, 0.7, 0.8, 0.9):
        sel = prob >= thr
        per = {}
        for s in src[sel]:
            fam = s.split("_")[0]
            per[fam] = per.get(fam, 0) + 1
        print(f"  prob >= {thr}: {int(sel.sum())} hits  {per}", flush=True)


def stats(a):
    z = np.load(a.scan, allow_pickle=False)
    print("model", z["model"], "scan min_prob", float(z["min_prob"]))
    stats_of(z["src"], z["prob"])


def build(a):
    """Store the selected hits as an end-aligned 224-frame negative set."""
    from mmap_ninja.ragged import RaggedMmap

    z = np.load(a.scan, allow_pickle=False)
    src, idx, end, prob = z["src"], z["idx"], z["end"], z["prob"]
    order = np.argsort(-prob)
    chosen = []
    per_source = {}
    for k in order:
        if prob[k] < a.min_prob:
            break
        s = str(src[k])
        if per_source.get(s, 0) >= a.max_per_source:
            continue
        per_source[s] = per_source.get(s, 0) + 1
        chosen.append(k)
    chosen.sort(key=lambda k: (str(src[k]), int(idx[k]), int(end[k])))
    out = FEAT / "mined_neg" / "training" / f"{a.tag}_mmap"
    done = out.parent / (out.name + ".done")
    if done.exists():
        raise SystemExit(f"exists: {out} (delete it and its .done to rebuild)")
    if out.exists():
        import shutil

        shutil.rmtree(out)
    out.parent.mkdir(parents=True, exist_ok=True)

    def gen():
        cache = {}
        for k in chosen:
            s = str(src[k])
            if s not in cache:
                cache[s] = RaggedMmap(str(SOURCES[s]))
            feats = np.asarray(cache[s][int(idx[k])])
            e = min(feats.shape[0], int(end[k]) + END_MARGIN)
            w = feats[max(0, e - KEEP_FRAMES):e]
            if w.shape[0] < KEEP_FRAMES:
                w = np.pad(w, ((KEEP_FRAMES - w.shape[0], 0), (0, 0)))
            yield w.astype(np.uint16)

    RaggedMmap.from_generator(out_dir=str(out), sample_generator=gen(), batch_size=256, verbose=False)
    done.touch()
    n = len(RaggedMmap(str(out)))
    log(f"built {out}: {n} windows (prob >= {a.min_prob}, max {a.max_per_source}/source) {per_source}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan")
    s.add_argument("model")
    s.add_argument("out")
    s.add_argument("--min-prob", type=float, default=0.3)
    s.add_argument("--workers", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    s.set_defaults(fn=scan)
    b = sub.add_parser("build")
    b.add_argument("scan")
    b.add_argument("tag")
    b.add_argument("--min-prob", type=float, default=0.5)
    b.add_argument("--max-per-source", type=int, default=100000)
    b.set_defaults(fn=build)
    t = sub.add_parser("stats")
    t.add_argument("scan")
    t.set_defaults(fn=stats)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
