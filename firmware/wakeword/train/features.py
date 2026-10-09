#!/usr/bin/env python3
"""Stage 4: augment the generated clips and build microWakeWord spectrogram features.

Runs in venv-mww. Writes RaggedMmap folders (uint16 features, 10 ms step, 40 channels:
the same TFLM micro frontend the device runs; uint16 = float feature * 25.6, exactly) under
$WW_FEAT, in the layout microWakeWord's FeatureHandler expects:

  positives/{training,validation}/hey_vesper_mmap        (truth=True)
  hardneg/{training,validation}/{hardneg,general}_mmap   (truth=False)
  audioset_neg/training/*_mmap                            (truth=False; AudioSet bal_train + train noise pool)
  val_ambient/validation_ambient -> CHiME-6 dev+eval      (symlink; DipCo is NOT linked)

Test-split features are NOT built here; eval.py builds them with the test-only augmentation.

Usage: features.py [positives] [hardneg] [audioset_neg] [val_ambient]   (default: all)
"""

import os
import random
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import augment_cfg  # noqa: E402

FEAT = Path(os.environ["WW_FEAT"])
GEN = Path(os.environ["WW_GEN"])
DATA = Path(os.environ["WW_DATA"])
SEED = 21

# Frames kept from the end of each 3.2 s augmented positive. The model needs
# spectrogram_length = 204 frames (clip_duration_ms 1500 + 154 slices dropped by the mixednet
# convolutions); +SLIDE-1 for the shifted copies, + a little margin.
KEEP_FRAMES_POS = 224
SLIDE_POS = 10


def seed_all(tag):
    s = abs(hash((SEED, tag))) % (1 << 31)
    random.seed(s)
    np.random.seed(s)


def to_u16(spec):
    return np.round(np.asarray(spec, dtype=np.float32) * 25.6).astype(np.uint16)


def build(out_dir, gen):
    from mmap_ninja.ragged import RaggedMmap

    done = out_dir.parent / (out_dir.name + ".done")
    if done.exists():
        print("exists, skipping:", out_dir, flush=True)
        return
    if out_dir.exists():  # a partial build from an interrupted run
        import shutil

        shutil.rmtree(out_dir)
    out_dir.parent.mkdir(parents=True, exist_ok=True)
    RaggedMmap.from_generator(out_dir=str(out_dir), sample_generator=gen, batch_size=256, verbose=True)
    done.touch()
    n = len(RaggedMmap(str(out_dir)))
    print(f"built {out_dir} ({n} spectrograms)", flush=True)


def clip_iter(directory, repeat=1):
    from microwakeword.audio.clips import Clips

    clips = Clips(input_directory=str(directory), file_pattern="**/*.wav", random_split_seed=None)
    print(f"{directory}: {len(clips.clips)} clips", flush=True)
    return clips.audio_generator(repeat=repeat)


def positive_specs(directory, augmenter, repeat):
    from microwakeword.audio.audio_utils import generate_features_for_clip

    for audio in clip_iter(directory, repeat):
        spec = generate_features_for_clip(augmenter.augment_clip(audio), step_ms=10)
        spec = spec[-KEEP_FRAMES_POS:]
        length = spec.shape[0] - SLIDE_POS + 1
        for i in range(SLIDE_POS):  # the word end shifted 0..9 frames earlier (upstream slide_frames)
            yield to_u16(spec[i:i + length])


def negative_specs(directory, augmenter, repeat):
    from microwakeword.audio.audio_utils import generate_features_for_clip

    for audio in clip_iter(directory, repeat):
        yield to_u16(generate_features_for_clip(augmenter.augment_clip(audio), step_ms=10))


def positives():
    for split, msplit, rep in (("train", "training", augment_cfg.POS_REPEAT), ("val", "validation", 1)):
        seed_all(f"pos-{split}")
        aug = augment_cfg.train_augmenter()
        build(FEAT / "positives" / msplit / "hey_vesper_mmap", positive_specs(GEN / "pos" / split, aug, rep))


def hardneg():
    for split, msplit, rep in (("train", "training", augment_cfg.NEG_REPEAT), ("val", "validation", 1)):
        for kind in ("hardneg", "general"):
            seed_all(f"{kind}-{split}")
            aug = augment_cfg.train_augmenter()
            build(FEAT / "hardneg" / msplit / f"{kind}_mmap", negative_specs(GEN / kind / split, aug, rep))


# AudioSet *bal_train* shards turned straight into negative features (training only). Shard 02 is
# the test-augmentation noise pool and is never used here; the evaluation split is never used.
AUDIOSET_NEG_SHARDS = [f"{i:02d}" for i in range(3, 13)]


def audioset_neg():
    """Everyday-sound negatives: AudioSet bal_train 03-12 (downloaded, featurised, parquet deleted)
    plus the train noise pool (AudioSet bal_train 00-01 + FMA-xs) as whole clips."""
    import download_data
    from microwakeword.audio.audio_utils import generate_features_for_clip

    def shard_specs():
        for shard in AUDIOSET_NEG_SHARDS:
            for _, pcm in download_data.audioset_shard_clips("bal_train", shard):
                yield to_u16(generate_features_for_clip(pcm, step_ms=10))
            print("audioset_neg shard", shard, flush=True)

    build(FEAT / "audioset_neg" / "training" / "audioset_bal_train_03_12_mmap", shard_specs())

    def pool_specs():
        from scipy.io import wavfile

        for wav in sorted((DATA / "noise_train").glob("*.wav")):
            sr, pcm = wavfile.read(str(wav))
            if len(pcm) >= 16000 * 2:
                yield to_u16(generate_features_for_clip(pcm, step_ms=10))

    build(FEAT / "audioset_neg" / "training" / "noise_train_pool_mmap", pool_specs())


def val_ambient():
    link = FEAT / "val_ambient" / "validation_ambient" / "chime6_dev_eval_mmap"
    if not link.exists():
        link.parent.mkdir(parents=True, exist_ok=True)
        link.symlink_to(DATA / "negative_datasets" / "dinner_party_eval" / "validation_ambient" / "chime6_dev_eval_mmap")
    print("val_ambient ->", link.resolve(), flush=True)


STEPS = {"positives": positives, "hardneg": hardneg, "audioset_neg": audioset_neg, "val_ambient": val_ambient}

if __name__ == "__main__":
    for s in sys.argv[1:] or list(STEPS):
        STEPS[s]()
    print("features done", flush=True)
