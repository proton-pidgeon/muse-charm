# Task 10: TTS playback slot (F2)

**Goal:** The node speaks Vesper's replies: the existing MP3→16 kHz decode path is opened and played through the speaker.
**Depends on:** 09
**Relevant specs:** [`specs/01-vesper-node-architecture.md`](../specs/01-vesper-node-architecture.md), [`specs/00-conflicts.md`](../specs/00-conflicts.md) (C4)
**Source:** `docs/vesper-node-architecture.md` §1, §2, §7 F2
**Est. effort:** not stated in source

## Deliverables
- [ ] Fill the empty TTS slot (`muse_chat_session.cpp:1503-1517` in stock; the equivalent in the new backend): fetch the per-message TTS MP3 URL
- [ ] Wire `tts_data` / `decode` / `muse_hatch_turn_read` so `K_TTS` actually opens and plays; `reply()` in `voice.c` plays `muse_hatch_turn_read()` PCM to the 16 kHz speaker
- [ ] Captions stay in sync with audio (stock `start_tts()` paced captions over silence)

## Definition of done
- [ ] On device: PTT question → spoken Vesper reply heard from the speaker
- [ ] `main/voice.c` still unchanged

## Anti-deliverables (do NOT build in this task)
- Changes to `voice.c` / `voice_player.c`
- Streaming TTS beyond the per-message MP3 URL design

## Risks / unknowns
- Backend MP3 is `mp3_22050_32` while the decode path targets 16 kHz output. Verify resampling in `decode`
- HUMAN GATE: needs Kevin to listen on device

## Notes for Claude Code
- Start by reading the linked `specs/` files and, if needed, the original source doc.
- Tick off boxes in this file as you complete them.
- If a deliverable is blocked, append `> blocked: <reason>` under it rather than removing it.
- Commit incrementally; one logical change per commit.
