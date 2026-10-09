# Task 21: Train custom "Hey Vesper" wake-word model (microWakeWord, Studio-local)

## Goal
Train a custom on-device wake-word model for "Hey Vesper" using microWakeWord (TensorFlow Lite Micro, ~50KB — proven on ESP32-S3 via ESPHome). Fully local, free, no cloud, no voice recordings: synthetic training samples generated with Piper TTS. When the model meets quality bars, it replaces the "Computer" bootstrap (task 20) in the firmware.

## Context
- Kevin researched this directly with Vesper (2026-10-08): ESP-SR's built-ins don't include "Vesper"; microWakeWord trains on synthetic Piper samples, free and reproducible.
- This runs on the Studio (mac-daddy31337), NOT on the board. Output is a .tflite model (~50KB) the firmware can load.
- Task 20 ("Computer" via WakeNet) is the live bootstrap; this task runs after it (one task per host).

## Steps
1. Set up the training environment on the Studio: microWakeWord repo + TensorFlow + Piper TTS with multiple voices.
2. Generate synthetic training set for "Hey Vesper":
   - Multiple Piper voices, varied pitch/speed.
   - Negative samples: similar-sounding phrases ("hey whisper", "a vesper", background speech), plus noise/reverb augmentation.
   - Document the dataset composition (counts per class).
3. Train the microWakeWord model targeting ~50KB (quantized int8 TFLite Micro).
4. Evaluate against quality bars (define these explicitly before training):
   - False-reject rate on held-out "Hey Vesper" samples (target: <5%).
   - False-accept rate on negatives + background audio (target: <1 false alarm per hour of background).
   - Model size ≤ 64KB.
   - If bars aren't met, iterate on data (more voices, harder negatives) — document what was tried.
5. Export the final .tflite + a short integration note (input format: sample rate, window size, how the firmware feeds it) to `firmware/wakeword/hey-vesper.tflite` (or the path task 20's implementer designates).
6. Do NOT swap it into the firmware in this task — that swap is a separate step after Kevin hears the quality numbers. Record the model + metrics in HANDOVER.md.

## Done when
- `hey-vesper.tflite` exported with documented quality metrics meeting the bars.
- Training reproducible: dataset recipe + training command recorded in the repo.
- HANDOVER.md updated; board issue closed with dotted Summary.

## Constraints
- Studio only. No cloud services, no real voice recordings — synthetic only.
- No secrets in logs. No firmware changes in this task (model + metrics only).
