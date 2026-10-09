"""Augmentation settings, one place for features.py (train/val) and eval.py (test).

Train/val draw RIRs and noise ONLY from the train pools. The test conditions draw ONLY from the
test pools (rirs_test, noise_test = AudioSet bal_train shard 02), which nothing else touches.
"""

import os
from pathlib import Path

DATA = Path(os.environ["WW_DATA"])
DURATION_S = 3.2          # augmented clip length (upstream notebook)
JITTER_S = (0.195, 0.205)  # phrase ends ~0.2 s before the clip end (upstream notebook)

POS_REPEAT = 1   # augmented copies per train positive (each also slid over 10 frames)
NEG_REPEAT = 2   # augmented copies per train hard-negative / general TTS clip

# upstream basic_training_notebook probabilities
TRAIN_PROBS = {
    "SevenBandParametricEQ": 0.1,
    "TanhDistortion": 0.1,
    "PitchShift": 0.1,
    "BandStopFilter": 0.1,
    "AddColorNoise": 0.1,
    "AddBackgroundNoise": 0.75,
    "Gain": 1.0,
    "RIR": 0.5,
}
TRAIN_SNR_DB = (-5, 10)


def train_augmenter():
    from microwakeword.audio.augmentation import Augmentation

    return Augmentation(
        augmentation_duration_s=DURATION_S,
        augmentation_probabilities=TRAIN_PROBS,
        impulse_paths=[str(DATA / "rirs_train")],
        background_paths=[str(DATA / "noise_train")],
        background_min_snr_db=TRAIN_SNR_DB[0],
        background_max_snr_db=TRAIN_SNR_DB[1],
        min_jitter_s=JITTER_S[0],
        max_jitter_s=JITTER_S[1],
    )


# Test conditions (quality-bars.md). "noisy" is the B1 bar condition.
TEST_CONDITIONS = {
    "clean": dict(probs={}, snr=(5, 15), gain=(0, 0)),
    "noisy": dict(probs={"RIR": 0.5, "AddBackgroundNoise": 0.75, "Gain": 1.0}, snr=(5, 15), gain=(-20, 0)),
    "hard": dict(probs={"RIR": 1.0, "AddBackgroundNoise": 1.0, "Gain": 1.0}, snr=(0, 5), gain=(-20, 0)),
}


def test_augmenter(condition):
    from microwakeword.audio.augmentation import Augmentation

    c = TEST_CONDITIONS[condition]
    return Augmentation(
        augmentation_duration_s=DURATION_S,
        augmentation_probabilities=c["probs"],
        impulse_paths=[str(DATA / "rirs_test")],
        background_paths=[str(DATA / "noise_test")],
        background_min_snr_db=c["snr"][0],
        background_max_snr_db=c["snr"][1],
        min_gain_db=c["gain"][0],
        max_gain_db=c["gain"][1],
        min_jitter_s=JITTER_S[0],
        max_jitter_s=JITTER_S[1],
    )
