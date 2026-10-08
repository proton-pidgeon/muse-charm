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
- [ ] Working transcript: 15-turn history per node, survives restarts, prepended to /ask; "tell me more about that" works within a session (manual or automated test as proof)
- [ ] Session awareness: 15-min idle compresses to a summary; summary seeds the next session
- [ ] Voice room assignment: "you're in the office" updates the registry and confirms by voice; no false positives on incidental mentions (tests as proof)
- [ ] Part 4 spec written (`docs/node-memory-integration.md`); explicitly NOT implemented
- [ ] Green build + adversarial review per the /implement gates; merged to main
- [ ] Backend redeployed (launchd) with the new code; live PTT test confirms context works

## Anti-deliverables (do NOT build in this task)
- Part 4 implementation (spec only)
- Wake-word work (separate track)
- Changes to the brain's /ask API or the Stage 1 brain's memory system
