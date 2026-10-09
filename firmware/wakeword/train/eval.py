#!/usr/bin/env python3
"""Stage 6: score a "Hey Vesper" .tflite under the frozen protocol in ../quality-bars.md.

  * the int8 streaming .tflite, run through the TFLite interpreter one [1,3,40] int8 frame
    group at a time (every 30 ms), uint8 probability out
  * features from the TFLM micro frontend (pymicro_features), streamed continuously per track
  * firmware decision rule: sum(last W uint8 outputs) > round(cutoff*255) * W; then 25
    inferences of cooldown. The first 25 inferences of every clip/track are ignored (frontend
    settling), as upstream does
  * operating point (cutoff, W) chosen on VALIDATION ONLY, then applied once to TEST

Usage:
  eval.py <model.tflite> [--out metrics.json] [--label NAME] [--fixed-cutoff C --fixed-window W]
          [--workers N]
  --fixed-* skips the selection and scores the given point (e.g. to re-check the shipped model).

Builds and caches every eval feature set under $WW_FEAT/eval/ (first run only).
"""

import argparse
import hashlib
import json
import math
import os
import re
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

FEAT = Path(os.environ["WW_FEAT"])
GEN = Path(os.environ["WW_GEN"])
DATA = Path(os.environ["WW_DATA"])
CACHE = FEAT / "eval"
SEED = 21

STRIDE = 3                  # feature frames per inference (model's first-layer stride)
STEP_S = 0.010              # feature step
COOLDOWN = 25               # inferences ignored after a detection (and at the start of a track)
WINDOWS = [3, 5, 7, 10]
CUTOFFS = [round(0.50 + 0.01 * i, 2) for i in range(50)]  # 0.50 .. 0.99
VAL_FAPH_TARGET = 0.5       # half of bar B2
BARS = {"frr_max": 0.05, "faph_max": 1.0, "bytes_max": 65536}

# ambient sources: name -> (split, kind, location)
AMBIENT = {
    "val": {
        "chime6_dev_eval": ("mmap", DATA / "negative_datasets/dinner_party_eval/validation_ambient/chime6_dev_eval_mmap"),
        "audioset_eval_30": ("wav", DATA / "background_val/audioset_eval_30.wav"),
        "audioset_eval_31": ("wav", DATA / "background_val/audioset_eval_31.wav"),
    },
    "test": {
        "dipco": ("mmap", DATA / "negative_datasets/dinner_party_eval/testing_ambient/dipco_u01_ch1_mmap"),
        **{f"audioset_eval_{s}": ("wav", DATA / f"background_test/audioset_eval_{s}.wav")
           for s in ("00", "01", "02", "03", "04", "05")},
    },
}


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


# ------------------------------------------------------------------------------------------
# features (main process; imports TF-dependent microwakeword lazily)


def u16(spec):
    return np.round(np.asarray(spec, dtype=np.float32) * 25.6).astype(np.uint16)


def features_of(pcm_int16):
    from microwakeword.audio.audio_utils import generate_features_for_clip

    return u16(generate_features_for_clip(pcm_int16, step_ms=10))


def _wav_track_features(path):
    from scipy.io import wavfile

    sr, pcm = wavfile.read(str(path))
    assert sr == 16000 and pcm.dtype == np.int16, path
    return features_of(pcm)


def ambient_tracks(split, workers):
    """{source: [uint16 [T,40] arrays]} for the split's ambient sources (cached)."""
    out = {}
    todo = []
    for name, (kind, loc) in AMBIENT[split].items():
        if kind == "mmap":
            from mmap_ninja.ragged import RaggedMmap

            r = RaggedMmap(str(loc))
            out[name] = [np.asarray(r[i]) for i in range(len(r))]
        else:
            cache = CACHE / "ambient" / f"{name}.npy"
            if cache.exists():
                out[name] = [np.load(cache)]
            else:
                todo.append((name, loc, cache))
    if todo:
        with ProcessPoolExecutor(max_workers=min(workers, len(todo))) as ex:
            for (name, loc, cache), feats in zip(todo, ex.map(_wav_track_features, [t[1] for t in todo])):
                cache.parent.mkdir(parents=True, exist_ok=True)
                np.save(cache, feats)
                out[name] = [feats]
                log("built ambient features", name, feats.shape)
    return out


def clip_set(name, directory, augmenter_factory):
    """Augmented clip features for every wav under `directory` (cached), with file names."""
    cache = CACHE / f"{name}.npz"
    if cache.exists():
        z = np.load(cache, allow_pickle=True)
        return list(z["names"]), list(z["feats"])
    import random

    import librosa

    files = sorted(Path(directory).glob("**/*.wav"))
    s = int(hashlib.sha256(f"{SEED}-{name}".encode()).hexdigest(), 16) % (1 << 31)
    random.seed(s)
    np.random.seed(s)
    aug = augmenter_factory()
    names, feats = [], []
    for i, f in enumerate(files):
        audio, _ = librosa.load(str(f), sr=16000, mono=True)
        a = aug.augment_clip(audio)
        feats.append(features_of(a))
        names.append(f"{f.parent.name}/{f.name}")
        if i % 1000 == 0:
            log(name, i, "/", len(files))
    cache.parent.mkdir(parents=True, exist_ok=True)
    arr = np.empty(len(feats), dtype=object)
    arr[:] = feats
    np.savez(cache, names=np.array(names), feats=arr)
    return names, feats


def val_noisy_augmenter():
    """Validation uses the TEST condition's parameters but only the TRAIN RIR/noise pools."""
    import augment_cfg
    from microwakeword.audio.augmentation import Augmentation

    c = augment_cfg.TEST_CONDITIONS["noisy"]
    return Augmentation(
        augmentation_duration_s=augment_cfg.DURATION_S,
        augmentation_probabilities=c["probs"],
        impulse_paths=[str(DATA / "rirs_train")],
        background_paths=[str(DATA / "noise_train")],
        background_min_snr_db=c["snr"][0], background_max_snr_db=c["snr"][1],
        min_gain_db=c["gain"][0], max_gain_db=c["gain"][1],
        min_jitter_s=augment_cfg.JITTER_S[0], max_jitter_s=augment_cfg.JITTER_S[1],
    )


def eval_sets():
    import augment_cfg

    sets = {
        "val_pos": (GEN / "pos/val", val_noisy_augmenter),
        "val_hardneg": (GEN / "hardneg/val", val_noisy_augmenter),
        "val_general": (GEN / "general/val", val_noisy_augmenter),
        "test_hardneg": (GEN / "hardneg/test", lambda: augment_cfg.test_augmenter("noisy")),
        "test_general": (GEN / "general/test", lambda: augment_cfg.test_augmenter("noisy")),
    }
    for cond in augment_cfg.TEST_CONDITIONS:
        sets[f"test_pos_{cond}"] = (GEN / "pos/test", lambda c=cond: augment_cfg.test_augmenter(c))
    return sets


# ------------------------------------------------------------------------------------------
# streaming inference (worker processes; no TensorFlow import)

_MODEL = None


def _init_worker(model_path):
    global _MODEL
    _MODEL = Path(model_path).read_bytes()


def quantize_input(feats_u16, scale, zp):
    """uint16 frontend output -> int8 model input, as the firmware does it.

    float feature = u16 / 25.6; q = round(float / scale) + zp. For the usual microWakeWord input
    params (scale 0.1015625 = 26/256, zp -128) this is exactly ESPHome's integer form
    ((u16 * 256) + 333) // 666 - 128.
    """
    if abs(scale - 0.1015625) < 1e-9 and zp == -128:
        q = (feats_u16.astype(np.int64) * 256 + 333) // 666 - 128
    else:
        q = np.round(feats_u16.astype(np.float64) / 25.6 / scale) + zp
    return np.clip(q, -128, 127).astype(np.int8)


def stream(feats_u16):
    """Run one continuous track through a FRESH streaming model; returns uint8 outputs."""
    from ai_edge_litert.interpreter import Interpreter

    # A fresh interpreter per track = fresh streaming state (zeroed ring buffers), like a boot.
    it = Interpreter(model_content=_MODEL)
    it.allocate_tensors()
    inp = it.get_input_details()[0]
    outp = it.get_output_details()[0]
    scale, zp = inp["quantization"]
    q = quantize_input(feats_u16, scale, zp)
    n = q.shape[0] // STRIDE
    out = np.empty(n, dtype=np.uint8)
    for i in range(n):
        it.set_tensor(inp["index"], q[i * STRIDE:(i + 1) * STRIDE].reshape(inp["shape"]))
        it.invoke()
        out[i] = it.get_tensor(outp["index"]).reshape(-1)[0]
    return out


def run_all(model_path, tracks, workers):
    with ProcessPoolExecutor(max_workers=workers, initializer=_init_worker, initargs=(str(model_path),)) as ex:
        return list(ex.map(stream, tracks, chunksize=max(1, len(tracks) // (workers * 8) or 1)))


def split_long(tracks, max_frames=360000):
    """Split hour-long tracks into <=1 h pieces for parallelism (each piece starts cold: the
    first COOLDOWN inferences of a piece are ignored, so this only drops ~0.75 s per hour)."""
    out = []
    for t in tracks:
        for s in range(0, len(t), max_frames):
            piece = t[s:s + max_frames]
            if len(piece) >= STRIDE * (COOLDOWN + 10):
                out.append(piece)
    return out


# ------------------------------------------------------------------------------------------
# decision rule


def moving_sum(p, w):
    c = np.cumsum(np.concatenate([np.zeros(w, dtype=np.int64), p.astype(np.int64)]))
    return c[w:] - c[:-w]  # sum of the last w outputs at each step (zeros before the start)


def thr_u8(cutoff):
    return int(round(cutoff * 255))


def count_detections(p, w, cutoff):
    s = moving_sum(p, w)
    idx = np.nonzero(s > thr_u8(cutoff) * w)[0]
    n, next_ok = 0, COOLDOWN
    for i in idx:
        if i >= next_ok:
            n += 1
            next_ok = i + COOLDOWN + 1
    return n


def clip_triggers(p, w, cutoff):
    s = moving_sum(p, w)[COOLDOWN:]
    return bool(s.size and s.max() > thr_u8(cutoff) * w)


def faph(outputs_by_source, w, cutoff):
    det, hours, per = 0, 0.0, {}
    for src, outs in outputs_by_source.items():
        d = sum(count_detections(p, w, cutoff) for p in outs)
        h = sum(len(p) for p in outs) * STRIDE * STEP_S / 3600
        per[src] = {"false_accepts": d, "hours": round(h, 3), "fa_per_hour": round(d / h, 4)}
        det += d
        hours += h
    return det / hours, det, hours, per


def frr(outputs, w, cutoff):
    return 1.0 - sum(clip_triggers(p, w, cutoff) for p in outputs) / len(outputs)


# ------------------------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("--out")
    ap.add_argument("--label", default="")
    ap.add_argument("--fixed-cutoff", type=float)
    ap.add_argument("--fixed-window", type=int)
    ap.add_argument("--workers", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    a = ap.parse_args()
    model = Path(a.model).resolve()
    t0 = time.time()

    from ai_edge_litert.interpreter import Interpreter

    it = Interpreter(model_path=str(model))
    it.allocate_tensors()
    inp, outp = it.get_input_details()[0], it.get_output_details()[0]
    model_info = {
        "file": model.name,
        "bytes": model.stat().st_size,
        "sha256": hashlib.sha256(model.read_bytes()).hexdigest(),
        "input": {"shape": [int(x) for x in inp["shape"]], "dtype": np.dtype(inp["dtype"]).name,
                  "scale": float(inp["quantization"][0]), "zero_point": int(inp["quantization"][1])},
        "output": {"shape": [int(x) for x in outp["shape"]], "dtype": np.dtype(outp["dtype"]).name,
                   "scale": float(outp["quantization"][0]), "zero_point": int(outp["quantization"][1])},
    }
    assert model_info["input"]["shape"][1] == STRIDE and model_info["input"]["dtype"] == "int8"
    log("model", model_info)

    # ---- features (cached) ----
    sets = {k: clip_set(k, d, f) for k, (d, f) in eval_sets().items()}
    amb = {split: ambient_tracks(split, a.workers) for split in ("val", "test")}

    # ---- inference ----
    outs = {}
    for k, (names, feats) in sets.items():
        outs[k] = run_all(model, feats, a.workers)
        log("scored", k, len(feats))
    amb_out = {}
    for split, by_src in amb.items():
        amb_out[split] = {}
        for src, tracks in by_src.items():
            amb_out[split][src] = run_all(model, split_long(tracks), a.workers)
            log("scored ambient", split, src)

    # ---- operating point (validation only) ----
    curves = {}
    if a.fixed_cutoff is not None:
        op = {"cutoff": a.fixed_cutoff, "window": a.fixed_window, "rule": "fixed by caller"}
    else:
        best = None
        for w in WINDOWS:
            for c in CUTOFFS:
                f, _, _, _ = faph(amb_out["val"], w, c)
                if f <= VAL_FAPH_TARGET:
                    r = frr(outs["val_pos"], w, c)
                    cand = (r, -w, c)
                    curves[str(w)] = {"cutoff": c, "val_faph": round(f, 4), "val_frr": round(r, 4)}
                    if best is None or cand < best:
                        best = cand
                    break
        if best is None:
            op = {"cutoff": None, "window": None, "rule": "no cutoff <= 0.99 reached val FA/h <= 0.5"}
        else:
            op = {"cutoff": best[2], "window": -best[1],
                  "rule": "per W: smallest cutoff with val FA/h <= 0.5; pick W with lowest val FRR (tie: larger W)"}
    log("operating point", op, curves)

    result = {"label": a.label, "model": model_info, "operating_point": op, "selection_curves": curves,
              "bars": BARS, "protocol": "firmware/wakeword/quality-bars.md"}
    if op["cutoff"] is not None:
        c, w = op["cutoff"], op["window"]
        vf, vd, vh, vper = faph(amb_out["val"], w, c)
        tf_, td, th, tper = faph(amb_out["test"], w, c)
        result["validation"] = {
            "frr_noisy": round(frr(outs["val_pos"], w, c), 4), "n_pos": len(outs["val_pos"]),
            "fa_per_hour": round(vf, 4), "false_accepts": vd, "background_hours": round(vh, 3), "per_source": vper,
            "hardneg_trigger_rate": round(1 - frr(outs["val_hardneg"], w, c), 4),
        }
        test = {"n_pos": len(outs["test_pos_noisy"])}
        for cond in ("clean", "noisy", "hard"):
            test[f"frr_{cond}"] = round(frr(outs[f"test_pos_{cond}"], w, c), 4)
        # FRR by voice source for the bar condition
        names = sets["test_pos_noisy"][0]
        by_src = {}
        for n, p in zip(names, outs["test_pos_noisy"]):
            by_src.setdefault(n.split("/")[0], []).append(p)
        test["frr_noisy_by_source"] = {k: {"n": len(v), "frr": round(frr(v, w, c), 4)} for k, v in by_src.items()}
        test.update({"fa_per_hour": round(tf_, 4), "false_accepts": td, "background_hours": round(th, 3),
                     "per_source": tper})
        # hard negatives per phrase (held-out speakers, noisy condition)
        import phrases

        per_phrase = {}
        hn_names = sets["test_hardneg"][0]
        for n, p in zip(hn_names, outs["test_hardneg"]):
            m = re.search(r"_h(\d\d)_", n)
            ph = phrases.HARD_NEGATIVES[int(m.group(1))]
            d = per_phrase.setdefault(ph, [0, 0])
            d[0] += clip_triggers(p, w, c)
            d[1] += 1
        test["hardneg_trigger_rate"] = round(sum(v[0] for v in per_phrase.values()) / max(1, sum(v[1] for v in per_phrase.values())), 4)
        test["hardneg_per_phrase"] = {k: {"triggers": v[0], "n": v[1], "rate": round(v[0] / v[1], 4)}
                                      for k, v in per_phrase.items()}
        test["general_tts_trigger_rate"] = round(1 - frr(outs["test_general"], w, c), 4)
        result["test"] = test
        result["passes"] = {
            "B1_frr": test["frr_noisy"] < BARS["frr_max"],
            "B2_faph": test["fa_per_hour"] < BARS["faph_max"],
            "B3_bytes": model_info["bytes"] <= BARS["bytes_max"],
        }
        result["passes"]["all"] = all(result["passes"].values())
        # full test curve at the chosen window, for the README (reported, never used to choose)
        result["test_curve_at_window"] = [
            {"cutoff": cc, "frr_noisy": round(frr(outs["test_pos_noisy"], w, cc), 4),
             "fa_per_hour": round(faph(amb_out["test"], w, cc)[0], 4)}
            for cc in CUTOFFS[::5] + [0.97, 0.98, 0.99]
        ]
    result["seconds"] = round(time.time() - t0, 1)
    js = json.dumps(result, indent=2, sort_keys=False)
    print(js)
    if a.out:
        Path(a.out).write_text(js + "\n")


if __name__ == "__main__":
    main()
