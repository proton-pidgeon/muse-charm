# Task 20: Wake-word bootstrap — "Computer" via ESP-SR WakeNet (firmware)

## Goal
Add trigger-word activation to the AiPi Lite (ESP32-S3) using Espressif's ESP-SR WakeNet with the built-in `wn9_computer_tts` model ("Computer"). Saying "Computer" wakes the board exactly as if the PTT button had been pressed. Push-to-talk keeps working alongside it. This is the fast-path bootstrap; the custom "Hey Vesper" model (task 21) replaces it later.

## Context
- Kevin researched this directly with Vesper (2026-10-08): ESP-SR has ~17 English built-in wake words; "Computer" is one of them (`wn9_computer_tts`). This avoids custom training for the bootstrap.
- Current state: PTT-only. Mic pipeline already streams audio for PTT (see `firmware/hatch/muse_chat_vesper.c`). Board is ESP32-S3 (check exact SRAM/PSRAM in the bringup doc).
- Privacy story (matters — this may live in a bedroom): wake-word detection stays 100% on-device. No audio leaves the board until the wake word fires, then the normal PTT turn flow takes over (upload → STT → brain → TTS).
- Design decision (Kevin-approved): both PTT and wake-word always active; wake-word triggers the identical listening/turn flow as the button.

## Steps
1. Integrate ESP-SR WakeNet (`wn9_computer_tts`) into the firmware audio path:
   - Tap the mic stream (find the capture path used by PTT — I2S/PDM driver, sample rate, buffering).
   - Run WakeNet on the live mic feed at idle. On detection → enter the listening state (same as PTT press): caption/listening UI, capture utterance, run the normal turn flow.
   - PTT button path untouched and still functional.
2. Memory/CPU: verify WakeNet fits alongside the existing firmware (avatar rendering, Wi-Fi, HTTP). Measure or bound the added RAM/CPU. If it doesn't fit, report exactly what's over budget rather than silently degrading.
3. Sensitivity tuning: expose a tunable threshold; pick a default balancing false triggers vs misses. Document how to adjust it.
4. False-trigger handling: a wake with no following speech should time out gracefully back to idle (no phantom turns, no backend calls).
5. UX: on wake-word detection, show the listening state on the display (same as PTT) so Kevin gets visible feedback.
6. Build green (existing firmware tests + any new ones), adversarial review per the skill gates.
7. Flash to the real AiPi Lite and live-verify: say "Computer" → board enters listening → speak → Vesper replies. Also verify PTT still works and false triggers are rare in a normal room.

## Done when
- Firmware with WakeNet "Computer" merged to main (green build + review).
- Flashed to the real board; live "Computer" wake verified on-device; PTT verified still working.
- HANDOVER.md updated; board issue closed with dotted Summary.

## Constraints
- Firmware work on the Studio only (`phylax fleet creds` must read valid).
- No audio leaves the device before wake-word detection — verify this in the design, not just assert it.
- No secrets in logs. No custom training in this task (that's task 21).

## Status (impl-20-wakeword, branch `impl/20-wakeword`, firmware 1.0.3)
Design and numbers: `firmware/README.md`, *Wake word (task 20)*.

- [x] 1. WakeNet `wn9_computer_tts` in the AIPI build (patch `0007`, `hatch/vesper_wakenet.c`); mic tapped at `muse_voice.c` `idle_capture()` (16 kHz mono, 20 ms, I2S); runs at idle only; detection enters LISTENING and the normal turn flow; PTT untouched.
- [x] 2. Memory/CPU: image 2,035,712 -> 2,691,072 B (64% of the 4 MiB slot); model 291,038 B in the image (PSRAM via rodata); static internal +18,220 B; WakeNet runtime and detect time logged at boot and in `>status` (Espressif: ~3 ms per 32 ms chunk, at most ~9% of core 1). Internal RAM is the budget to watch; confirm on the device (human gate).
- [x] 3. Sensitivity: `CONFIG_VESPER_WAKE_THRESHOLD=650` (Espressif's tuning) plus `>wake.threshold=0.50-0.99` / `>wake=on|off` in NVS, clamped and validated (host-tested).
- [x] 4. False triggers: no backend call before a speech onset; no speech in 5 s means back to idle quietly with zero backend calls (state machine + host tests).
- [x] 5. UX: the same LISTENING mode and caption as PTT; the screen wakes.
- [x] 6. Build green: `make -C firmware test` (incl. `test_vesper_wake`, log hygiene); fresh `b1a3822` tree, `apply-sdk.sh` idempotent, AIPI build with 0 warnings. Adversarial review: orchestrator's gate.
- [ ] 7. **HUMAN GATE (on-device):** OTA 1.0.3 (`vesper-node firmware publish`; no USB flash and no partition change needed); read the boot `vesper_wake` lines (detect us, memory, free internal); say "Computer" + a question and get a reply; say "Computer" alone and see a quiet 5 s timeout with no `/turn` in the backend log; PTT turn plus a press during a wake listen; a false-trigger soak of a few hours in a normal room (`>wake` counts), tuning `>wake.threshold` if needed. See README *On-device check (task 20)*.
