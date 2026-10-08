# Task 17: Node shared long-term memory (Part 4)

**Goal:** Node conversations feed Vesper's long-term memory. Kevin approved Part 4 (2026-10-08 ~15:46 CDT): "Definitely want long term memory."
**Source:** Kevin's approval; design spec `docs/node-memory-integration.md` (task-16 implementer, 2026-10-08). **READ THE SPEC FIRST — it is the design. Implement it; do not redesign it.** If implementation reveals a gap, note it in the spec file as a comment for Kevin's review, but do not change the design unilaterally.
**Depends on:** 07, 08, 16 (backend, registry, conversation memory). The per-node transcript from task 16 is the input.
**Relevant code:** `backend/src/vesper_node/conversation.py` (ConversationStore), `backend/src/vesper_node/app.py` (turn pipeline), `backend/src/vesper_node/brain.py`.

## Principle (from the spec — non-negotiable)

**The backend proposes, Vesper disposes.** The node backend stages memory *candidates* in a queue. Vesper's main agent reviews, dedups, and curates them into the real memory on her normal cadence. **No direct writes to curated memory (`~/MEMORY.md`, `~/memory/*.md`) from the node pipeline — ever.**

## What to build

### 1. Significance detection (node backend)
- After each turn completes (async — must NOT add user-facing latency), judge whether the exchange is significant enough to propose for long-term memory.
- **Explicit (high confidence):** user says "remember this", "don't forget", "save that", "note that".
- **Heuristic (medium confidence, start conservative):** the turn contains a durable fact, preference, commitment, or event. Tight patterns only — a missed memory is a minor gap; a wrong memory pollutes the curated store.
- **Excluded:** small talk, follow-up questions ("tell me more"), transient state ("what's the weather"), anything ambiguous.
- Implementer's call on mechanism (lightweight classifier/patterns, or a dedicated brain `/ask`), but it must be cheap and async.

### 2. Candidate queue
- **Location:** `~/memory/node-candidates.jsonl` on the Studio (append-only, mode 600, never leaves the Studio).
- **Format:** one JSON object per line: `ts`, `node_id`, `room`, `user_text`, `vesper_reply`, `significance` (explicit|heuristic), `reason`, `status` (pending), `dedupe_key` (hash of normalized core fact).
- **Never stored:** audio bytes, full transcripts (candidate carries the single significant exchange or a summary only), anything not clearly Kevin's own statement, anything said by another person in the room.

### 3. Safety gates (from task-16 review — carry forward)
- **Memory writes must NEVER trigger actions.** Significance detection and candidate staging run OUTSIDE the brain's action-capable path. No turn text is ever fed to an action-capable endpoint for memory purposes (the task-16 reviewers blocked brain-based summarization for exactly this reason — "turn off the kitchen lights" must not re-fire).
- The candidate queue is write-only from the backend; nothing in the node pipeline reads curated memory or acts on it.

### 4. Review-side hook (Vesper's main agent)
- Document (in the spec file) the review contract: Vesper checks the queue on her normal cadence, dedups via `muse.memory_search`, writes to `MEMORY.md`/daily notes, marks candidates reviewed/rejected with reasons. No code needed on the agent side in this task — the contract documentation is the deliverable.

## Definition of done
- [ ] Significance detection: explicit triggers ("remember this") always queue; conservative heuristics queue durable facts; small talk/transient/ambiguous never queue (tests as proof)
- [ ] Candidate queue: append-only JSONL at `~/memory/node-candidates.jsonl`, mode 600, correct schema, dedupe_key present
- [ ] Async: significance judging adds zero latency to the turn path (test or measurement as proof)
- [ ] Safety: no memory-path code path can invoke home-control or any other action (reviewer-verified)
- [ ] Privacy: no audio, no full transcripts, no third-party statements in candidates (tests as proof)
- [ ] Review contract documented in `docs/node-memory-integration.md`
- [ ] Green build + adversarial review per the /implement gates; merged to main
- [ ] Backend redeployed (launchd); live turn with "remember this" produces a candidate

## Explicitly out of scope
- Vesper's main-agent review loop (that's her runtime, not this repo).
- Changing the spec's design without Kevin's approval.
- Direct writes to `~/MEMORY.md` or `~/memory/*.md` from the backend.
