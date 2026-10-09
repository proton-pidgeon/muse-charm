# Task 22: "Hey Vesper" wake word in firmware — microWakeWord integration (REVISED 2026-10-09 ~04:50 CDT)

## Goal
Replace the crashed "Computer" WakeNet bootstrap with the trained custom "Hey Vesper" microWakeWord model. Saying "Hey Vesper" wakes the board exactly as if the PTT button had been pressed. Push-to-talk keeps working alongside it.

## CRITICAL CONTEXT — READ FIRST
- **1.0.3 ("Computer" WakeNet) CRASHES ON BOOT.** Flashed to the real board 2026-10-09 ~04:50 CDT: boot-loops with `Guru Meditation Error: Core 1 panic'ed (LoadProhibited)` immediately after WakeNet loads the "Computer" model. This was the SRAM exhaustion risk flagged (but never verified) in task 20's HANDOVER. Board was rolled back to stable 1.0.2.
- **"Computer" (WakeNet) is DEAD — do NOT keep it as a fallback.** The keep-vs-replace decision from the original task file is settled: REPLACE. Remove the WakeNet integration entirely.
- **OTA NOW WORKS.** `hatch.host` was fixed to `https://peggy.fly.dev/vesper-node` during the same Studio visit; the board's boot log confirms `update check: up to date (HTTP 200; running 1.0.2, published 1.0.2)`. **When 1.0.4 is built and merged, PUBLISH it to the OTA store** (`vesper-node firmware publish`) — the board will pull it automatically. This supersedes the old "do not publish OTA" constraint.
- **SRAM is the hard constraint.** microWakeWord (TFLite Micro + 61KB model) has a very different memory profile from WakeNet (~291KB model + heavy runtime), which is why it may succeed where WakeNet failed — but this must be VERIFIED, not assumed. The task is not green until the memory budget is proven.

## Context
- Kevin approved "Add Hey Vesper" (2026-10-09 ~04:12 CDT) after the model passed all quality bars.
- Model: `firmware/wakeword/hey-vesper.tflite` (60,840 bytes), merged as `ef786b1`. Training recipe at `firmware/wakeword/train/`, metrics at `firmware/wakeword/metrics.json`, quality bars at `firmware/wakeword/quality-bars.md`.
- Previous firmware 1.0.3 (merged `8b9239c`, now abandoned): `hatch/vesper_wakenet.c`, mic tapped at `muse_voice.c` `idle_capture()` (16 kHz mono, 20 ms, I2S), threshold `CONFIG_VESPER_WAKE_THRESHOLD`, `>wake=on|off` and `>wake.threshold=` serial controls.
- Privacy story: wake-word detection stays 100% on-device. No audio leaves the board until the wake word fires.

## Steps
1. REMOVE the WakeNet "Computer" integration cleanly (`hatch/vesper_wakenet.c`, the `wn9_computer_tts` model blob, associated Kconfig). Keep the serial `>wake=on|off` and `>wake.threshold=` controls working, repointed at the new model.
2. Integrate microWakeWord (TensorFlow Lite Micro) inference for `hey-vesper.tflite`:
   - Reuse the existing mic tap (`muse_voice.c` `idle_capture()`, 16 kHz mono).
   - Run the TFLite model on the live mic feed at idle. On detection → enter LISTENING (same as PTT press): caption/listening UI, capture utterance, normal turn flow.
   - PTT button path untouched.
3. **Memory budget (the critical gate):** compute the worst-case SRAM/PSRAM footprint of TFLite Micro + the 61KB model + audio buffers, alongside the existing firmware (avatar, Wi-Fi, HTTP). Compare against the ESP32-S3's available internal SRAM. The WakeNet crash was a `LoadProhibited` on Core 1 at model load — identify exactly what allocation pattern caused it and prove microWakeWord avoids it. Report exact numbers. If it doesn't fit, STOP and report — do not ship a crash-looping firmware.
4. Sensitivity: expose a tunable threshold; default informed by training metrics (FRR 2.93%, 0.31 FA/h). Document adjustment.
5. False-trigger handling: wake with no following speech times out gracefully back to idle (no phantom turns, no backend calls).
6. UX: on "Hey Vesper" detection, show the listening state on the display (same as PTT).
7. Bump firmware version to 1.0.4.
8. Build green (existing firmware tests + any new ones), adversarial review per the skill gates. The review MUST include a memory-budget adversarial pass given the 1.0.3 crash.
9. **Publish 1.0.4 to the OTA store** (`vesper-node firmware publish`) after merge. The board (now on working OTA) will pull it automatically. Verify the board picks it up via the backend firmware-check log.
10. Update `docs/usb-flash-runbook.md`: note that OTA is now the delivery path; USB flash is fallback only.

## Done when
- Firmware 1.0.4 with "Hey Vesper" (WakeNet fully removed) merged to main (green build + review including memory-budget pass).
- 1.0.4 published to the OTA store.
- Board pulls 1.0.4 via OTA (verify via backend log: `firmware check` shows the board on 1.0.4).
- "Hey Vesper" wake verified working (via board logs or Kevin's live test).
- HANDOVER.md updated; board issue #20 closed with dotted Summary.

## Constraints
- Firmware work on the Studio only (`phylax fleet creds` must read valid).
- No audio leaves the device before wake-word detection.
- No secrets in logs.
- Known model confusables: "a vesper" 13.8%, "hey whisper" 8.8%, "hey Esther" 5% — Kevin accepted these.
- If microWakeWord cannot fit in SRAM either, do NOT force it — report back with the numbers and alternative approaches.

## Status (impl-22-wakeword-heyvesper-fw, branch `impl/22-wakeword-heyvesper-fw`, firmware 1.0.4)
Design and numbers: `firmware/README.md`, *Wake word (tasks 20, 22)*; model-side check: `firmware/wakeword/README.md`, *In the firmware*.

- [x] 1. microWakeWord on TFLite Micro on the idle mic tap (`idle_capture()`, 16 kHz mono, 20 ms): `hatch/vesper_wakeword_engine.cc` (microfrontend + TFLM), `hatch/vesper_mww.c` (quantiser, 3-frame stride, W 3 / cutoff / cooldown rule), `hatch/vesper_wakeword.c` (glue); patch `0008`. Detection enters LISTENING and the normal turn flow; PTT untouched.
- [x] 2. "Computer" REPLACED: esp-sr, WakeNet glue and `SR_*` config removed; `>wake` controls kept for the new model. Why: keeping both costs WakeNet's ~18 KB static + ~16 KB run-time internal RAM, ~2.95 MB image, two detectors on one feed, an added false-wake rate.
- [x] 3. Memory/CPU measured: image 2,691,072 -> 2,297,856 B (45% of slot free); static internal 191,133 -> 173,105 B (+192 B over 1.0.2); ext .bss +4,208 B; arena 32 KB PSRAM (24,608 B used on host); ~24,800 MACs / 30 ms inference (≤ ~3.5% of core 1 bound). Board figures: boot log (human gate).
- [x] 4. Threshold `CONFIG_VESPER_WAKE_THRESHOLD=650` (task 21's validated point), window 3; serial `>wake.threshold=`, NVS `muse:wake_hv_thr`. Documented with the test curve and confusables.
- [x] 5. False triggers: task 20's listen path, unchanged (no backend before onset; 5 s quiet timeout).
- [x] 6. UX: same LISTENING mode/caption as PTT; screen wakes.
- [x] 7. `hatch/VERSION` 1.0.4.
- [x] 8. Build green: `make -C firmware test` (incl. new `test_vesper_mww`, log hygiene); fresh `b1a3822` tree, `apply-sdk.sh` idempotent (and upgrades a 1.0.3 tree with just `0008`), AIPI build OK (0 warnings in Vesper/Muse code; 8 -Wshadow inside esp-tflite-micro's own SUB kernel). `make -C firmware wake-host-check` passes (firmware pipeline == training pipeline). Adversarial review: orchestrator's gate.
- [x] 9. Not flashed, not published.
- [x] 10. `docs/usb-flash-runbook.md` flashes 1.0.4; binary `/Users/k3v/builds/muse-charm/scratch/firmware-1.0.4/muse-gadget.bin` (also recorded in HANDOVER.md).
- [ ] Done-when items left to the orchestrator/Kevin: merge after review, board issue close, on-device check (`firmware/README.md`, *On-device check (task 22)*).
