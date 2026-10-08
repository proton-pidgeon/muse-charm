# Task 10: TTS playback slot (F2)

**Goal:** The node speaks Vesper's replies: the existing MP3→16 kHz decode path is opened and played through the speaker.
**Depends on:** 09
**Relevant specs:** [`specs/01-vesper-node-architecture.md`](../specs/01-vesper-node-architecture.md), [`specs/00-conflicts.md`](../specs/00-conflicts.md) (C4)
**Source:** `docs/vesper-node-architecture.md` §1, §2, §7 F2
**Est. effort:** not stated in source

## Deliverables
- [x] Fill the empty TTS slot (`muse_chat_session.cpp:1503-1517` in stock; the equivalent in the new backend): fetch the per-message TTS MP3 URL
  - `firmware/hatch/muse_chat_vesper.c`: `vesper_tts_slot_offer()` keeps the URL that `vp_resolve_audio_url` resolved (always on the configured server). `start_showing()`, the stock `start_tts()`, then GETs it: same bearer and `X-Node-Id`, `Accept: audio/mpeg`, no redirects, 5 s connect/headers timeout. It needs 200 + `audio/mpeg` and at most 2 MiB. The URL is never logged.
  - Per-message only: no streaming TTS beyond the message_done URL.
- [x] Wire `tts_data` / `decode` / `muse_hatch_turn_read` so `K_TTS` actually opens and plays; `reply()` in `voice.c` plays `muse_hatch_turn_read()` PCM to the 16 kHz speaker
  - The stock `tts_data`/`decode` path, rebuilt on the hatch task:
    - flow-controlled 20 ms reads into a 192 KiB PSRAM buffer;
    - minimp3 with the stock 1,445-byte hold;
    - stereo downmix;
    - the result into the same `s_out` stream buffer.
  - On the AIPI, the unchanged `muse_voice.c` `hatch_reply()` (the AIPI's equivalent of `voice.c`'s `reply()`) reads that buffer with `muse_hatch_turn_read()` and writes it to the 16 kHz speaker.
  - `voice.c` and `voice_player.c` are byte-identical; `apply-sdk.sh` checks `voice.c`.
  - Resampling (risk below): `mp3_22050_32` → 16 kHz used to be stock's linear interpolation, which aliases 8-11 kHz into the speech (a 10 kHz tone came out only 4.6 dB down, host test). It is now an anti-aliased polyphase windowed-sinc filter (`firmware/hatch/vesper_audio.c`: 48 taps, exact rational time base), host-tested:
    - 1 kHz at gain 1.0000 with 63 dB SNR;
    - the 10 kHz alias at -78 dB;
    - 3 s in → 3 s out;
    - identical output for any split.

    On device, 50 frames x 576 samples at 22,050 Hz became 20,898 samples at 16 kHz: 1.306 s → 1.306 s.
  - The only SDK-side change is one line in `sdk-patches/0003` that builds `vesper/vesper_audio.c`.
- [x] Captions stay in sync with audio (stock `start_tts()` paced captions over silence)
  - `pcm_frames` comes from the audio: while decoding, samples out plus bytes left at the measured bytes per sample (the frame header's bitrate is wrong: the first frame claims 56 kbps in a 32 kbps file); once drained, exact. Silence is no longer used when speech exists.
  - Fallbacks:
    - no URL, or a fetch/HTTP/type/decode failure: stock silent pacing;
    - a cut-short or stalled download: plays what came, then paces the rest of the caption.

    A failed TTS never wedges the turn.
  - On device: the page turned at 0.95 s of 1.31 s (72%), at character 15 of 21 (71%).
  - Cancel: a new press or `turn_cancel` drains `s_out`, and `turn_finish()` closes the GET. Every write is checked against the turn generation, so a stale turn's audio never reaches the speaker. Barge-in was verified on device.

## Definition of done
- [ ] On device: PTT question → spoken Vesper reply heard from the speaker
  > blocked: HUMAN GATE. The agent cannot hear the speaker.

  Serial evidence (2026-10-08, AiPi on `/dev/cu.usbmodem83201`, build from `~/builds/muse-charm/scratch/sdk-impl-10`, host `http://192.168.5.16:8797`, `>hatch.test` → `server check: HTTP 200`, question "what is two plus two"):
  - `turn: HTTP 200`, then `transcript: 21 chars`, then `message 0 done (21 chars, with speech)`;
  - `tts: message 0: GET HTTP 200, Content-Length 5347, audio/mpeg, headers in 23 ms`, then `fetched 5347 of 5347 bytes in 26 ms`;
  - `reply audio 22050 Hz, 1 ch ... -> 16000 Hz`, then `50 MP3 frames at 22050 Hz -> 20898 samples at 16000 Hz (1.31 s of speech); first audio 57 ms after the GET`;
  - `muse_voice: muse reply: 1.31s of audio`: PCM written to the speaker;
  - `caption: message 0 page 2 at 0.95 s of 1.31 s (char 15 of 21)`.

  A second turn played 0.71 s ("Four."). The barge-in test logged `reply interrupted` and `turn cancelled`, with no leftover audio. No PSRAM leak: the heartbeat showed 5,223 KiB free again. The token never appears in any log. The full logs are in `/tmp/t10-device.log`.

  **Listen steps for Kevin:** the board is still flashed and provisioned.
  1. Make sure `com.vesper.node` (`:8796`) and the socat forward (`lsof -nP -iTCP:8797`) are up.
  2. Hold the talk button, say "what is two plus two", and release.
  3. Within about 5-8 s you should hear Vesper's voice from the AiPi's speaker, with the caption paging along with it.
  4. If it's silent but the serial log shows `muse reply: X s of audio` with X > 0, check Muse's volume setting.
- [x] `main/voice.c` still unchanged
  - `apply-sdk.sh` reports `ok: esp32/main/voice.c unchanged from b1a3822`.
  - The task-09 kept-byte-identical list is untouched: `voice_player.c`, `board_aipi.c`, `voice_board.c`, the LVGL UI, `muse_voice.c`, `muse_wifi`, `identity.c`, `ota.c`.

## Anti-deliverables (do NOT build in this task)
- Changes to `voice.c` / `voice_player.c`
- Streaming TTS beyond the per-message MP3 URL design

## Risks / unknowns
- Backend MP3 is `mp3_22050_32` while the decode path targets 16 kHz output. Verify resampling in `decode`
  - Verified and replaced (see above): stock linear interpolation aliased badly; `vesper_audio.c` is anti-aliased and keeps the duration exactly (host tests plus on-device sample counts).
- HUMAN GATE: needs Kevin to listen on device

## Notes for Claude Code
- Start by reading the linked `specs/` files and, if needed, the original source doc.
- Tick off boxes in this file as you complete them.
- If a deliverable is blocked, append `> blocked: <reason>` under it rather than removing it.
- Commit incrementally; one logical change per commit.
