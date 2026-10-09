# Task 22: "Hey Vesper" wake word in firmware — microWakeWord integration

## Goal
Replace (or supplement) the "Computer" WakeNet bootstrap with the trained custom "Hey Vesper" microWakeWord model. Saying "Hey Vesper" wakes the board exactly as if the PTT button had been pressed. Push-to-talk keeps working alongside it.

## Context
- Kevin approved "Add Hey Vesper" (2026-10-09 ~04:12 CDT) after the model passed all quality bars.
- Model: `firmware/wakeword/hey-vesper.tflite` (60,840 bytes), merged as `ef786b1`. Training recipe at `firmware/wakeword/train/`, metrics at `firmware/wakeword/metrics.json`, quality bars at `firmware/wakeword/quality-bars.md`.
- Current firmware 1.0.3 (merged `8b9239c`) has ESP-SR WakeNet "Computer" embedded. Task 20 details: `hatch/vesper_wakenet.c`, mic tapped at `muse_voice.c` `idle_capture()` (16 kHz mono, 20 ms, I2S), threshold `CONFIG_VESPER_WAKE_THRESHOLD`, `>wake=on|off` and `>wake.threshold=` serial controls. WakeNet "Computer" model is ~291KB in PSRAM via rodata.
- Board OTA is BROKEN (HTTP hatch.host vs HTTPS-only gate — needs Kevin at Studio for USB flash). Do NOT publish OTA in this task. The USB flash runbook is at `docs/usb-flash-runbook.md` — update it to reference 1.0.4.
- Privacy story: wake-word detection stays 100% on-device. No audio leaves the board until the wake word fires.

## Design decision (Kevin-approved framing)
"Hey Vesper" is the PRIMARY wake word. Keep "Computer" (WakeNet) as a fallback ONLY if the combined memory/compute budget allows cleanly — the hey-vesper model is only ~61KB vs WakeNet's ~291KB, so both may fit. If keeping both degrades RAM/CPU headroom or complicates the audio path, REPLACE "Computer" with "Hey Vesper" and document why. Document the final choice in HANDOVER.md and `firmware/README.md`.

## Steps
1. Integrate microWakeWord (TensorFlow Lite Micro) inference for `hey-vesper.tflite` into the firmware audio path:
   - Reuse the existing mic tap (`muse_voice.c` `idle_capture()`, 16 kHz mono).
   - Run the TFLite model on the live mic feed at idle. On detection → enter LISTENING (same as PTT press / WakeNet detection): caption/listening UI, capture utterance, normal turn flow.
   - PTT button path untouched.
2. Decide Computer keep-vs-replace per the design decision above. If kept: both detectors run at idle; either can trigger listening. If replaced: remove the WakeNet integration cleanly (keep the serial `>wake` controls working for the new model).
3. Memory/CPU: verify the TFLite Micro runtime + 61KB model fits alongside everything else (avatar, Wi-Fi, HTTP, and WakeNet if kept). Measure or bound added RAM/CPU. Report exact numbers; if over budget, report what's over rather than silently degrading.
4. Sensitivity: expose a tunable threshold for the new model; pick a default informed by the training metrics (FRR 2.93%, 0.31 FA/h). Document how to adjust.
5. False-trigger handling: wake with no following speech times out gracefully back to idle (no phantom turns, no backend calls) — same as task 20.
6. UX: on "Hey Vesper" detection, show the listening state on the display (same as PTT/WakeNet).
7. Bump firmware version to 1.0.4.
8. Build green (existing firmware tests + any new ones), adversarial review per the skill gates.
9. Do NOT flash the real board (Kevin is not at the Studio; USB flash is his Studio-visit step). Do NOT publish OTA.
10. Update `docs/usb-flash-runbook.md`: the Studio visit now flashes 1.0.4 (with "Hey Vesper") instead of 1.0.3. Note the new binary location.

## Done when
- Firmware 1.0.4 with "Hey Vesper" merged to main (green build + review).
- Built binary location recorded in HANDOVER.md (for the USB flash).
- USB-flash runbook updated to 1.0.4.
- HANDOVER.md updated; board issue closed with dotted Summary.
- The Computer keep-vs-replace decision documented with the numbers behind it.

## Constraints
- Firmware work on the Studio only (`phylax fleet creds` must read valid).
- No audio leaves the device before wake-word detection.
- No OTA publish. No secrets in logs.
- Known model confusables (from task 21): "a vesper" 13.8%, "hey whisper" 8.8%, "hey Esther" 5% — Kevin accepted these; no need to re-litigate, but the threshold default should reflect them.

## Status (impl-22-wakeword-heyvesper-fw, branch `impl/22-wakeword-heyvesper-fw`, firmware 1.0.4)
