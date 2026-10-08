# Task 16: Node conversation memory + voice room assignment

**Goal:** The node remembers conversational context. Kevin noticed "tell me more about that headline" fails — each turn is amnesiac.
**Source:** Kevin's request 2026-10-08 ~15:10 CDT.
**Depends on:** 06, 07, 08 (node channel, backend, registry). Dispatch after task 15 finishes (one task per host).
**Relevant code:** `backend/src/vesper_node/app.py` (turn pipeline), `backend/src/vesper_node/brain.py` (BrainClient.ask — sends `{"text", "device_id", "channel"}`), `backend/src/vesper_node/registry.py` (`set_room`).

## Part 1 — Working transcript (IMPLEMENT)
- Backend keeps the last ~15 turns per `node_id` (user text + Vesper reply). Store alongside the registry or in a dedicated per-node transcript file — implementer's call, but it must survive backend restarts (the backend died 2026-10-08 and lost in-memory state; don't repeat that).
- On each `/turn`, prepend the transcript as conversation history to the brain `/ask` text (same pattern as the existing room-context prepend via `with_room_context`).
- Keep the prepend compact: recent turns verbatim, older turns truncated. Stay well under the `/ask` 1000-char limit (refuse or summarize rather than clip, per the existing rule).
- Privacy: transcripts are Kevin's own voice; store on the Studio only, never log full text (follow the existing log-hygiene: status codes and char counts, not content).

## Part 2 — Session awareness (IMPLEMENT)
- Track last-turn timestamp per node. After ~15 minutes of idle, don't drop the transcript — compress it to a short summary.
- On the next turn after idle: summarize the old transcript (a single brain `/ask` for summarization, or an extractive fallback — implementer's call), store the summary as the session seed, clear the transcript.
- The summary is prepended (like the transcript) so "what were we talking about" still works across the gap.

## Part 3 — Voice room assignment (IMPLEMENT, small)
- Kevin wants to say "you're in the office" (or "this is the kitchen", "call this the study") and have the node update its registry room + confirm by voice.
- Detect the intent in the node backend (explicit patterns — implementer's call on regex vs. lightweight NLU, but be conservative: only fire on clear assignment phrasing, never on incidental room mentions like "is the office light on").
- On match: call `registry.set_room(node_id, room)`, and return a voice confirmation ("Got it — this is the office now.") without needing a brain round-trip for the confirmation itself. Still send the turn to the brain for a natural response if preferred — implementer's call, but the room update must happen.
- Validate the room name with the existing `valid_room` rules.

## Part 4 — Shared long-term memory (SPEC ONLY — DO NOT IMPLEMENT)
- Design the integration, don't build it. This touches Vesper's shared memory (the Stage 1 brain's curated memory), so Kevin reviews the design first.
- The spec must cover:
  - **What counts as "significant"**: heuristics for which node conversations merit long-term memory (explicit "remember this", vs. LLM-judged significance). Who decides — backend, brain, or Vesper herself?
  - **The handoff mechanism**: propose a queue (e.g., `~/memory/node-candidates.jsonl` or a brain endpoint) where the node backend stages memory candidates. Vesper's main agent reviews and curates them into the real memory — the backend proposes, Vesper disposes. Direct writes to the curated memory are OUT.
  - **Dedup/merge**: how candidates avoid duplicating what's already known.
  - **Privacy**: what's eligible (Kevin's home conversations) and what's never stored.
- The spec is ALREADY WRITTEN: `docs/node-memory-integration.md` (coordinator, 2026-10-08). READ IT — do not rewrite it. If implementation reveals a design gap, note it in the file as a comment for Kevin's review, but do not change the design unilaterally. No code for Part 4 in this task.

## Definition of done
- [x] Working transcript: 15-turn history per node, survives restarts, prepended to /ask; "tell me more about that" works within a session (manual or automated test as proof)
- [x] Session awareness: 15-min idle compresses to a summary; summary seeds the next session
- [x] Voice room assignment: "you're in the office" updates the registry and confirms by voice; no false positives on incidental mentions (tests as proof)
- [x] Part 4 spec written (`docs/node-memory-integration.md`); explicitly NOT implemented
- [ ] Green build + adversarial review per the /implement gates; merged to main
- [ ] Backend redeployed (launchd) with the new code; live PTT test confirms context works

## Implementation notes (implement/16-node-conversation-memory)

- **Where state lives:** `backend/src/vesper_node/conversation.py` (`ConversationStore`). One
  JSON file, default `~/.config/vesper-voice/node-memory.json` (next to `nodes.json`), keyed by
  node id: last 15 turns `{user, reply, ts}`, `summary`, `last_ts`. Written like the registry:
  temp file + fsync + `os.replace` + dir fsync, mode 600 in a 700 dir; read with `O_NOFOLLOW`;
  a malformed/group-readable file is ignored (logged by name) and replaced on the next write.
  The service is the only writer, so its RAM copy is authoritative after the first load; a
  failed write is logged and the turn still succeeds (memory stays in RAM until a restart).
  Timestamps are wall-clock so idle detection survives restarts. Hand-built test settings
  (`memory_file=None`) keep memory in RAM only, so no test touches `~/.config`.
- **Config knobs:** `VESPER_NODE_MEMORY_FILE`, `VESPER_NODE_SESSION_IDLE_MINUTES` (default 15,
  1-1440). Both shown by `vesper-node check-config` and stripped by `make test` (keyless).
- **Prompt shape:** `[Vesper node in the <room>] [Earlier session summary: …] [Recent
  conversation, oldest first: Kevin: … / Vesper: … // …] <utterance>`. With no history the
  text is exactly the task-08 form. History block ≤ 640 chars (`HISTORY_BUDGET`) and always
  fits what is left of the 1000-char limit after the room prefix + utterance: newest 3 turns up
  to 320 chars per side, older ones 60 per side, newest first until the budget runs out, then
  the summary if it fits. The utterance is never clipped: if room prefix + utterance alone is
  over 1000 the turn is still refused as `transcript_too_long`.
- **What is recorded:** an exchange is appended only after `/ask` returns a reply (TTS failure
  still records it; a failed/cancelled ask records nothing; a brain fallback reply such as
  "Sorry, …" is a 200 and is recorded as a Vesper turn — known, harmless). Room-assignment turns are not
  recorded (no brain reply; the room shows up in the room prefix anyway).
- **Concurrency:** a per-node `asyncio.Lock` guards the read + idle-compression step and the
  append step; it is released during the main `/ask` so a slow brain doesn't block the node's
  next turn. Append re-reads under the lock, so overlapping turns both land (tested).
- **Session awareness:** on the first turn more than the idle gap after `last_ts`, the old
  transcript is compressed into an **extractive** summary ("Kevin asked: <last 3 questions>.
  Vesper last said: <first sentence of the last reply>"), built on the backend with no brain
  call. The implementer's-call option of "a single brain `/ask` for summarization" was tried
  and dropped at review: the brain's `/ask` always runs the home-control tool loop and ignores
  the channel, so a summary request whose body is old utterances ("turn off the kitchen
  lights") could act on them, a node-side timeout only cancels the node's wait (the brain keeps
  running), and a brain fallback string arrives as HTTP 200 and would be stored as the
  summary. Summary ≤ 300 chars; transcript cleared; the summary seeds every later turn until
  the next compression replaces it. (See the design-gap notes at the end of
  `docs/node-memory-integration.md` for the brain-side change that would enable an LLM
  summary safely.)
- **Voice room assignment:** `backend/src/vesper_node/roomcmd.py`. Whole-utterance regexes
  only (leading fillers like "hey Vesper," / "okay" stripped; a trailing "?" never fires).
  Open-vocabulary names (anything `valid_room` accepts) only behind an unambiguous naming
  marker: "this room is (now) called X", "this room is (now) the X", "call this room the X",
  "call this room Kevin's lab" (possessive), "set your/the/this room to the X", "your room is
  (now) the X". Everything else — the ordinary-English phrasings ("you're in the X", "this is
  the X", "call this the X") and the marker-less *room* phrasings ("this room is X", "call
  this room X", "set the room to X", "your room is X") — fires only when the name ends in a
  room noun (office, kitchen, study, den, bedroom, workshop, …), so "this room is cold", "set
  the room to seventy", "call this room service", "you're in the way" never match (review
  finding, fixed). Numbers/measurements ("70 degrees", "72") and names ending in a
  state/thermostat word ("dark", "a mess", "the worst", "cool") are rejected even behind a
  marker and go to the brain. Max 3 words; apostrophes dropped, hyphens to spaces,
  lower-cased, then `valid_room`.
  On a match the registry is updated and "Got it — this is the <room> now." is spoken via the
  normal `message_*` events + TTS, with no brain call. **Invalid name decision:** the turn
  does not fall through to the brain; the node speaks a short refusal ("Sorry, I can't use
  that as a room name…") and the room is unchanged. A registry write failure ends the turn
  with the generic `internal` error.
- **Logs:** counts, char lengths and labels only (`memory=<n>[+summary]|room` on `turn done`,
  `session compressed: … summary=extractive chars=N`). Tests check with caplog that no
  utterance, reply or summary text appears.
- **Docs:** `docs/node-wire-protocol.md` (Q4 amended, pipeline step 4, operator section,
  changelog). Part 4 (`docs/node-memory-integration.md`) left as written; nothing implemented.
- **Tests:** `backend/tests/test_conversation.py`, `backend/tests/test_roomcmd.py`, plus config
  tests in `test_config.py`.

## Anti-deliverables (do NOT build in this task)
- Part 4 implementation (spec only)
- Wake-word work (separate track)
- Changes to the brain's /ask API or the Stage 1 brain's memory system
