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
