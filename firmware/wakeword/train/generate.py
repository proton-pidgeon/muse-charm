#!/usr/bin/env python3
"""Stage 3: synthesise the "Hey Vesper" dataset with Piper TTS (runs in venv-piper).

Three TTS sources, split by SPEAKER/VOICE so no held-out voice is ever trained on:

  lt    the multi-speaker LibriTTS-R generator (en_US-libritts_r-medium.pt, 904 speakers).
        Speakers 0..799 are shuffled with a fixed seed: 80 -> test, 60 -> val, 660 -> train.
        Each sample SLERP-mixes two speakers of the SAME split.
  onnx  stock Piper voices (.onnx). Whole single-speaker voices are assigned to train or test;
        multi-speaker voices (vctk, l2arctic, arctic, aru) are split by speaker id.
  xl    (train only, it5) non-English stock Piper voices speaking English phonemes (XL_VOICES).

Synthesis settings (speed = length_scale, noise_scale, noise_w) also differ: test uses values
that are never used for train/val (see SETTINGS).

Usage: generate.py <split> <kind> [--scale F]
  split: train | val | test        kind: pos | hardneg | general
Writes $WW_GEN/<kind>/<split>/{lt,onnx}/*.wav + manifest.csv. Re-runnable (resumes by index).
"""

import argparse
import csv
import itertools as it
import json
import os
import random
import sys
import wave
from pathlib import Path

import numpy as np
import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import phrases  # noqa: E402

GEN = Path(os.environ["WW_GEN"])
SRC = Path(os.environ["WW_SRC"])
VOICES_DIR = Path(os.environ["WW_DATA"]) / "piper_voices"
PT_MODEL = SRC / "piper-sample-generator" / "models" / "en_US-libritts_r-medium.pt"
HF_VOICES = "https://huggingface.co/rhasspy/piper-voices/resolve/main"
SEED = 21

# --- speaker splits -------------------------------------------------------------------------
LT_SPEAKERS = list(range(800))  # piper-sample-generator README: later speakers are artefact-prone
random.Random(SEED).shuffle(LT_SPEAKERS)
LT_SPLIT = {"test": LT_SPEAKERS[:80], "val": LT_SPEAKERS[80:140], "train": LT_SPEAKERS[140:]}

# single-speaker voices: whole voice in one split
ONNX_SINGLE = {
    "train": [
        "en_US-lessac-medium", "en_US-amy-medium", "en_US-ryan-medium", "en_GB-alan-medium",
        "en_GB-southern_english_female-low", "en_US-hfc_female-medium", "en_US-joe-medium",
        "en_US-john-medium", "en_US-bryce-medium", "en_GB-cori-medium", "en_US-ljspeech-medium",
        "en_US-danny-low", "en_US-kathleen-low",
    ],
    "val": [],
    "test": [
        "en_US-kristin-medium", "en_US-hfc_male-medium", "en_GB-northern_english_male-medium",
        "en_GB-jenny_dioco-medium", "en_US-kusal-medium", "en_US-norman-medium",
        "en_GB-alba-medium", "en_US-mike-medium", "en_US-sam-medium", "en_US-reza_ibrahim-medium",
    ],
}
# multi-speaker voices: (voice, n_speakers); speakers split test/val/train by a seeded shuffle
ONNX_MULTI = [("en_GB-vctk-medium", 109), ("en_US-l2arctic-medium", 24),
              ("en_US-arctic-medium", 18), ("en_GB-aru-medium", 12), ("en_GB-semaine-medium", 4)]
MULTI_FRACTION = {"test": 0.2, "val": 0.1}


def onnx_voice_speakers(split):
    """[(voice, speaker_id)] for an onnx split."""
    out = [(v, 0) for v in ONNX_SINGLE[split]]
    for voice, n in ONNX_MULTI:
        ids = list(range(n))
        random.Random(f"{SEED}-{voice}").shuffle(ids)
        n_test = max(1, round(n * MULTI_FRACTION["test"]))
        n_val = max(1, round(n * MULTI_FRACTION["val"])) if n >= 8 else 0
        parts = {"test": ids[:n_test], "val": ids[n_test:n_test + n_val], "train": ids[n_test + n_val:]}
        out += [(voice, s) for s in parts[split]]
    return out


# --- synthesis settings: test values never appear in train/val --------------------------------
SETTINGS = {
    "train": dict(length_scales=[0.75, 0.85, 1.0, 1.1, 1.25, 1.4], noise_scales=[0.5, 0.667, 0.8, 0.9, 1.0],
                  noise_ws=[0.5, 0.8, 1.0], slerp=[0.0, 0.25, 0.5, 0.75, 1.0]),
    "val": dict(length_scales=[0.75, 0.85, 1.0, 1.1, 1.25, 1.4], noise_scales=[0.5, 0.667, 0.8, 0.9, 1.0],
                noise_ws=[0.5, 0.8, 1.0], slerp=[0.0, 0.25, 0.5, 0.75, 1.0]),
    "test": dict(length_scales=[0.8, 0.93, 1.18, 1.33], noise_scales=[0.6, 0.72, 0.95],
                 noise_ws=[0.65, 0.9], slerp=[0.35, 0.6]),
}

# --- per-split counts (scaled by --scale) ----------------------------------------------------
COUNTS = {  # kind -> split -> (n_lt, n_onnx, n_xl)  [hardneg counts are PER PHRASE]
    "pos": {"train": (40000, 15000, 15000), "val": (1000, 500, 0), "test": (1500, 1500, 0)},  # it3: lt/onnx (16000, 6000) -> (40000, 15000); it5: + xl
    "hardneg": {"train": (400, 150, 100), "val": (25, 10, 0), "test": (40, 40, 0)},
    "general": {"train": (3000, 1500, 1000), "val": (150, 50, 0), "test": (300, 300, 0)},
}

# it5: "xl" = non-English Piper voices fed ENGLISH (en-us espeak) phonemes, i.e. accented English
# from ~30 more voice models (several multi-speaker). TRAIN ONLY: val/test voices are unchanged.
XL_VOICES = [
    "de_DE-mls-medium", "fr_FR-mls-medium", "nl_NL-mls-medium", "de_DE-thorsten-medium",
    "de_DE-thorsten_emotional-medium", "es_ES-sharvard-medium", "es_ES-davefx-medium",
    "es_MX-ald-medium", "it_IT-paola-medium", "it_IT-serena-medium", "fr_FR-siwis-medium",
    "fr_FR-tom-medium", "fr_FR-upmc-medium", "nl_NL-pim-medium", "pl_PL-darkman-medium",
    "pl_PL-gosia-medium", "pt_BR-faber-medium", "pt_BR-cadu-medium", "ru_RU-denis-medium",
    "ru_RU-irina-medium", "sv_SE-nst-medium", "no_NO-nvcc-medium", "hi_IN-pratham-medium",
    "hi_IN-priyamvada-medium", "cy_GB-bu_tts-medium", "da_DK-talesyntese-medium",
    "cs_CZ-kasandra-medium", "hu_HU-anna-medium",
]
# English phonemes some foreign voices lack -> closest sequences they have
XL_FALLBACK = {"ɚ": ["ə", "ɹ"], "ɝ": ["ɜ", "ɹ"], "ɹ": ["r"], "ᵻ": ["ɪ"], "ɾ": ["t"], "ɐ": ["a"],
               "ʌ": ["a"], "ɜ": ["ə"], "ɪ": ["i"], "ʊ": ["u"], "æ": ["a"], "ɑ": ["a"], "ɔ": ["o"]}


def texts_for(kind):
    return {"pos": phrases.POSITIVE, "hardneg": phrases.HARD_NEGATIVES, "general": phrases.GENERAL}[kind]


def write_wav(path, pcm, sr):
    with wave.open(str(path), "wb") as w:
        w.setframerate(sr)
        w.setsampwidth(2)
        w.setnchannels(1)
        w.writeframes(pcm.tobytes())


# --- LibriTTS-R generator --------------------------------------------------------------------
class LTGenerator:
    def __init__(self):
        from piper_sample_generator.__main__ import generate_audio, get_phonemes, audio_float_to_int16

        self.generate_audio, self.get_phonemes, self.to_int16 = generate_audio, get_phonemes, audio_float_to_int16
        self.model = torch.load(PT_MODEL, weights_only=False)
        self.model.eval()
        self.device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
        self.model.to(self.device)
        self.config = json.load(open(f"{PT_MODEL}.json"))
        self.sr = self.config["audio"]["sample_rate"]

    def batch(self, texts, spk1, spk2, slerp, length, noise, noise_w, seed):
        torch.manual_seed(seed)
        ids = [self.get_phonemes(self.config["espeak"]["voice"], self.config, t) for t in texts]
        m = max(len(x) for x in ids)
        ids = [x + [1] * (m - len(x)) for x in ids]
        with torch.no_grad():
            audio, ph = self.generate_audio(self.model, torch.LongTensor(spk1), torch.LongTensor(spk2), ids,
                                            slerp, noise, noise_w, length, None)
            for i in range(audio.shape[0]):
                audio[i, 0, int(ph[i].flatten().sum().item()) + 1:] = 0
            a = audio.cpu().numpy()
        if self.device.type == "mps":
            torch.mps.empty_cache()
        out = []
        for i in range(a.shape[0]):
            out.append(np.trim_zeros(self.to_int16(a[i:i + 1])[0].flatten()))
        return out


def gen_lt(split, kind, n_total, outdir, writer):
    texts = texts_for(kind)
    spk = LT_SPLIT[split]
    st = SETTINGS[split]
    per_text = kind == "hardneg"
    targets = [(t, n_total) for t in texts] if per_text else [(None, n_total)]
    gen = None
    bs = 50
    for text, n in targets:
        tag = "" if text is None else f"h{texts.index(text):02d}_"
        idx = 0
        while idx < n:
            names = [outdir / f"lt_{tag}{idx + j:06d}.wav" for j in range(min(bs, n - idx))]
            if all(p.exists() for p in names):
                idx += len(names)
                continue
            rng = random.Random(f"{SEED}-{split}-{kind}-lt-{tag}{idx}")  # per-batch: resumable
            if gen is None:
                gen = LTGenerator()
            b = len(names)
            s1 = [rng.choice(spk) for _ in range(b)]
            s2 = [rng.choice(spk) for _ in range(b)]
            slerp = rng.choice(st["slerp"])
            length = rng.choice(st["length_scales"])
            noise = rng.choice(st["noise_scales"])
            noise_w = rng.choice(st["noise_ws"])
            tx = [text if text is not None else rng.choice(texts) for _ in range(b)]
            pcm = gen.batch(tx, s1, s2, slerp, length, noise, noise_w, seed=rng.randrange(1 << 30))
            for p, a, t, x, y in zip(names, pcm, tx, s1, s2):
                if len(a) < 1600:  # <0.1 s: a failed synthesis
                    continue
                write_wav(p, a, gen.sr)
                writer.writerow([p.name, t, "libritts_r", f"{x}+{y}", slerp, length, noise, noise_w])
            idx += b


# --- stock Piper voices (onnx) ---------------------------------------------------------------
def ensure_voice(voice):
    onnx = VOICES_DIR / f"{voice}.onnx"
    if onnx.exists() and (VOICES_DIR / f"{voice}.onnx.json").exists():
        return onnx
    lang, rest = voice.split("-", 1)
    name, quality = rest.rsplit("-", 1)
    base = f"{HF_VOICES}/{lang.split('_')[0]}/{lang}/{name}/{quality}/{voice}"
    VOICES_DIR.mkdir(parents=True, exist_ok=True)
    import subprocess

    for suf in (".onnx", ".onnx.json"):
        subprocess.run(["curl", "-fsSL", "--retry", "5", "-o", str(VOICES_DIR / f"{voice}{suf}"), base + suf], check=True)
    return onnx


def gen_onnx(split, kind, n_total, outdir, writer):
    from piper import PiperVoice, SynthesisConfig

    texts = texts_for(kind)
    vs = onnx_voice_speakers(split)
    if not vs:
        return
    st = SETTINGS[split]
    per_text = kind == "hardneg"
    targets = [(t, n_total) for t in texts] if per_text else [(None, n_total)]
    loaded = {}
    for text, n in targets:
        tag = "" if text is None else f"h{texts.index(text):02d}_"
        for idx in range(n):
            rng = random.Random(f"{SEED}-{split}-{kind}-onnx-{tag}{idx}")  # per-sample: resumable
            voice, sid = vs[rng.randrange(len(vs))]
            length = rng.choice(st["length_scales"])
            noise = rng.choice(st["noise_scales"])
            noise_w = rng.choice(st["noise_ws"])
            t = text if text is not None else rng.choice(texts)
            p = outdir / f"onnx_{tag}{idx:06d}.wav"
            if p.exists():
                continue
            if voice not in loaded:
                loaded[voice] = PiperVoice.load(str(ensure_voice(voice)))
            pv = loaded[voice]
            cfg = SynthesisConfig(speaker_id=sid if pv.config.num_speakers > 1 else None,
                                  length_scale=length, noise_scale=noise, noise_w_scale=noise_w)
            with wave.open(str(p), "wb") as w:
                pv.synthesize_wav(t, wav_file=w, syn_config=cfg)
            writer.writerow([p.name, t, voice, sid, "", length, noise, noise_w])


def xl_phoneme_ids(id_map, phonemes):
    """Phoneme ids for a foreign voice; None if a phoneme has no mapping even after fallbacks."""
    ids = list(id_map["^"]) + list(id_map["_"])
    for ph in phonemes:
        seq = [ph]
        while any(p not in id_map for p in seq):
            new = []
            for p in seq:
                if p in id_map:
                    new.append(p)
                elif p in XL_FALLBACK:
                    new.extend(XL_FALLBACK[p])
                elif p in ("ˈ", "ˌ", "ː"):  # stress/length marks: drop if unknown
                    continue
                else:
                    return None
            if new == seq:
                return None
            seq = new
        for p in seq:
            ids.extend(id_map[p])
            ids.extend(id_map["_"])
    ids.extend(id_map["$"])
    return ids


def gen_xl(split, kind, n_total, outdir, writer):
    if n_total <= 0:
        return
    from piper import PiperVoice, SynthesisConfig
    from piper.phonemize_espeak import EspeakPhonemizer
    from piper_sample_generator.__main__ import audio_float_to_int16

    phon = EspeakPhonemizer()
    texts = texts_for(kind)
    st = SETTINGS[split]
    per_text = kind == "hardneg"
    targets = [(t, n_total) for t in texts] if per_text else [(None, n_total)]
    loaded, ph_cache = {}, {}
    for text, n in targets:
        tag = "" if text is None else f"h{texts.index(text):02d}_"
        for idx in range(n):
            rng = random.Random(f"{SEED}-{split}-{kind}-xl-{tag}{idx}")  # per-sample: resumable
            voice = XL_VOICES[rng.randrange(len(XL_VOICES))]
            length = rng.choice(st["length_scales"])
            noise = rng.choice(st["noise_scales"])
            noise_w = rng.choice(st["noise_ws"])
            t = text if text is not None else rng.choice(texts)
            spk_draw = rng.random()
            p = outdir / f"xl_{tag}{idx:06d}.wav"
            if p.exists():
                continue
            if voice not in loaded:
                loaded[voice] = PiperVoice.load(str(ensure_voice(voice)))
            pv = loaded[voice]
            if t not in ph_cache:
                ph_cache[t] = [ph for sent in phon.phonemize("en-us", t) for ph in sent]
            ids = xl_phoneme_ids(pv.config.phoneme_id_map, ph_cache[t])
            if ids is None:
                print("xl: skip", voice, repr(t), flush=True)
                continue
            sid = int(spk_draw * pv.config.num_speakers) if pv.config.num_speakers > 1 else None
            cfg = SynthesisConfig(speaker_id=sid, length_scale=length, noise_scale=noise, noise_w_scale=noise_w)
            audio = pv.phoneme_ids_to_audio(ids, cfg)
            pcm = audio_float_to_int16(np.asarray(audio)[np.newaxis, :]).flatten()
            if len(pcm) > 2.6 * pv.config.sample_rate:  # would lose its start in the 3.2 s clip
                continue
            write_wav(p, pcm, pv.config.sample_rate)
            writer.writerow([p.name, t, voice, sid if sid is not None else 0, "", length, noise, noise_w])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("split", choices=["train", "val", "test"])
    ap.add_argument("kind", choices=["pos", "hardneg", "general"])
    ap.add_argument("--scale", type=float, default=1.0, help="multiply the COUNTS table")
    ap.add_argument("--only", choices=["lt", "onnx", "xl"], default=None)
    a = ap.parse_args()
    n_lt, n_onnx, n_xl = (int(round(c * a.scale)) if c else 0 for c in COUNTS[a.kind][a.split])
    base = GEN / a.kind / a.split
    for src, n, fn in (("lt", n_lt, gen_lt), ("onnx", n_onnx, gen_onnx), ("xl", n_xl, gen_xl)):
        if n <= 0:
            continue
        if a.only and a.only != src:
            continue
        outdir = base / src
        outdir.mkdir(parents=True, exist_ok=True)
        man = base / f"manifest_{src}.csv"
        new = not man.exists()
        with open(man, "a", newline="") as f:
            w = csv.writer(f)
            if new:
                w.writerow(["file", "text", "voice", "speaker", "slerp", "length_scale", "noise_scale", "noise_w"])
            fn(a.split, a.kind, n, outdir, w)
        print(f"{a.kind}/{a.split}/{src}: {len(list(outdir.glob('*.wav')))} wavs", flush=True)


if __name__ == "__main__":
    main()
