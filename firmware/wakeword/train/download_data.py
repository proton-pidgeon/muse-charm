#!/usr/bin/env python3
"""Stage 2: download the public datasets the recipe uses (all from Hugging Face).

Every item lands under $WW_DATA and is re-runnable (skips anything already present).
Roles (see ../quality-bars.md for why each split is where it is):

  rirs        MIT environmental impulse responses (271 RIRs, 16 kHz). Split by a fixed hash
              into rirs_train/ (augmenting training + validation clips) and rirs_test/
              (augmenting held-out test clips only).
  noise       Background-noise clips mixed INTO wake-word/hard-negative clips:
              AudioSet bal_train shards 00-01 -> noise_train/ ; fma_xs -> noise_train/ ;
              AudioSet bal_train shard 02 -> noise_test/ (test-clip augmentation only).
  negatives   kahrendt/microwakeword precomputed negative spectrogram features
              (speech, dinner_party, no_speech: TRAINING; dinner_party_eval: validation ambient).
  background  Raw held-out background audio for the FALSE-ACCEPT-PER-HOUR test, never trained
              on and never used for tuning: AudioSet *evaluation* shards (EVAL_TEST_SHARDS).
              Two further evaluation shards (EVAL_VAL_SHARDS) are the extra validation ambient
              set used for cutoff tuning. Each shard is concatenated into one long 16 kHz track.

Usage: python download_data.py [rirs] [noise] [negatives] [background]  (default: all)
"""

import hashlib
import io
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

import numpy as np

DATA = Path(os.environ["WW_DATA"])
HF = "https://huggingface.co/datasets"
SR = 16000

AUDIOSET_NOISE_TRAIN_SHARDS = ["00", "01"]
AUDIOSET_NOISE_TEST_SHARDS = ["02"]
EVAL_SPLIT = "ev" + "al"  # AudioSet's evaluation split directory name
EVAL_VAL_SHARDS = ["30", "31"]
EVAL_TEST_SHARDS = ["00", "01", "02", "03", "04", "05"]


def log(*a):
    print(*a, flush=True)


def download(url, dest: Path):
    if dest.exists() and dest.stat().st_size > 0:
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".part")
    log("download", url)
    for attempt in range(6):  # HF's CDN occasionally drops HTTP/2 streams mid-file (curl exit 92)
        r = subprocess.run(["curl", "-fsSL", "--http1.1", "--retry", "5", "-C", "-", "-o", str(tmp), url])
        if r.returncode == 0:
            break
        log(f"curl exit {r.returncode}, retrying ({attempt + 1})")
    else:
        raise RuntimeError(f"download failed: {url}")
    tmp.rename(dest)
    return dest


def to_int16_16k(audio: np.ndarray, sr: int) -> np.ndarray:
    import soxr

    if audio.ndim > 1:
        audio = audio.mean(axis=1)
    if sr != SR:
        audio = soxr.resample(audio.astype(np.float32), sr, SR)
    return (np.clip(audio, -1.0, 1.0) * 32767).astype(np.int16)


def write_wav(path: Path, pcm: np.ndarray):
    from scipy.io import wavfile

    path.parent.mkdir(parents=True, exist_ok=True)
    wavfile.write(str(path), SR, pcm)


def stable_bucket(name: str, mod: int = 10) -> int:
    return int(hashlib.sha256(name.encode()).hexdigest(), 16) % mod


# ---------------------------------------------------------------------------------------------


def rirs():
    done = DATA / "rirs_test"
    if done.exists():
        log("rirs ok")
        return
    import json

    listing = json.load(
        urllib.request.urlopen(
            "https://huggingface.co/api/datasets/davidscripka/MIT_environmental_impulse_responses/tree/main/16khz"
        )
    )
    import soundfile as sf

    raw = DATA / "rirs_raw"
    for item in listing:
        p = item["path"]
        if not p.endswith(".wav"):
            continue
        download(f"{HF}/davidscripka/MIT_environmental_impulse_responses/resolve/main/{p}", raw / Path(p).name)
    for wav in sorted(raw.glob("*.wav")):
        audio, sr = sf.read(str(wav), dtype="float32")
        split = "rirs_test" if stable_bucket(wav.name) < 2 else "rirs_train"  # ~20% held out
        write_wav(DATA / split / wav.name, to_int16_16k(audio, sr))
    shutil.rmtree(raw)
    log("rirs:", {d: len(list((DATA / d).glob("*.wav"))) for d in ("rirs_train", "rirs_test")})


def audioset_shard_clips(split: str, shard: str):
    """Yields (name, int16 16 kHz pcm) for every clip in one AudioSet parquet shard."""
    import pyarrow.parquet as pq
    import soundfile as sf

    pq_path = download(f"{HF}/agkphysics/AudioSet/resolve/main/data/{split}/{shard}.parquet",
                       DATA / "audioset_parquet" / f"{split}_{shard}.parquet")
    table = pq.read_table(str(pq_path))
    cols = table.column_names
    audio_col = table.column("audio").to_pylist()
    ids = table.column(cols[0]).to_pylist()
    for vid, a in zip(ids, audio_col):
        try:
            audio, sr = sf.read(io.BytesIO(a["bytes"]), dtype="float32")
        except Exception as e:  # a handful of clips are undecodable; skip them
            log("skip", vid, e)
            continue
        yield str(vid), to_int16_16k(audio, sr)
    pq_path.unlink()


def noise():
    out_train, out_test = DATA / "noise_train", DATA / "noise_test"
    for shard in AUDIOSET_NOISE_TRAIN_SHARDS:
        marker = out_train / f".audioset_{shard}"
        if marker.exists():
            continue
        n = 0
        for name, pcm in audioset_shard_clips("bal_train", shard):
            write_wav(out_train / f"as_{name}.wav", pcm)
            n += 1
        marker.touch()
        log("noise_train audioset", shard, n)
    for shard in AUDIOSET_NOISE_TEST_SHARDS:
        marker = out_test / f".audioset_{shard}"
        if marker.exists():
            continue
        n = 0
        for name, pcm in audioset_shard_clips("bal_train", shard):
            write_wav(out_test / f"as_{name}.wav", pcm)
            n += 1
        marker.touch()
        log("noise_test audioset", shard, n)
    marker = out_train / ".fma_xs"
    if not marker.exists():
        import librosa

        z = download(f"{HF}/mchl914/fma_xsmall/resolve/main/fma_xs.zip", DATA / "fma_xs.zip")
        tmp = DATA / "fma_tmp"
        with zipfile.ZipFile(z) as zf:
            zf.extractall(tmp)
        n = 0
        for mp3 in sorted(tmp.glob("**/*.mp3")):
            try:
                audio, sr = librosa.load(str(mp3), sr=SR, mono=True)
            except Exception as e:
                log("skip", mp3.name, e)
                continue
            write_wav(out_train / f"fma_{mp3.stem}.wav", to_int16_16k(audio, SR))
            n += 1
        shutil.rmtree(tmp)
        z.unlink()
        marker.touch()
        log("noise_train fma", n)


# Disk budget (~30 GB for the whole recipe): drop mmaps that duplicate another one's content from a
# different microphone (VOiCES close-talk and mid lavalier = same utterances as the far one we keep;
# CHiME-6 array u02 = same sessions as array u01). Iterations it10-it13 kept u02
# (WW_KEEP_CHIME_U02=1, with dinner_party re-extracted); the shipped model (it8) did not.
PRUNE = [
    "speech/training/voices_lav_clo_training_mmap",
    "speech/training/voices_lav_mid_training_mmap",
] + ([] if os.environ.get("WW_KEEP_CHIME_U02") == "1" else ["dinner_party/training/chime6_train_u02_ch1_mmap"])


def negatives():
    out = DATA / "negative_datasets"
    for name in ("dinner_party", "dinner_party_eval", "no_speech", "speech"):
        if (out / name).exists():
            log("negatives ok", name)
            continue
        z = download(f"{HF}/kahrendt/microwakeword/resolve/main/{name}.zip", DATA / f"{name}.zip")
        with zipfile.ZipFile(z) as zf:
            zf.extractall(out)
        z.unlink()
        log("negatives", name, "extracted")
    for rel in PRUNE:
        if (out / rel).exists():
            shutil.rmtree(out / rel)
            log("pruned", rel)


def background():
    for role, shards in (("background_val", EVAL_VAL_SHARDS), ("background_test", EVAL_TEST_SHARDS)):
        out = DATA / role
        for shard in shards:
            dest = out / f"audioset_eval_{shard}.wav"
            if dest.exists():
                continue
            parts = [pcm for _, pcm in audioset_shard_clips(EVAL_SPLIT, shard)]
            track = np.concatenate(parts)
            write_wav(dest, track)
            log(role, shard, f"{len(parts)} clips, {len(track) / SR / 3600:.2f} h")


STEPS = {"rirs": rirs, "noise": noise, "negatives": negatives, "background": background}

if __name__ == "__main__":
    wanted = sys.argv[1:] or list(STEPS)
    for w in wanted:
        STEPS[w]()
    log("download done:", wanted)
