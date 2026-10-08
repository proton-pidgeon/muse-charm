# Node ↔ Vesper Shared Long-Term Memory — Integration Spec

**Status:** DESIGN ONLY — Kevin reviews before any implementation.
**Date:** 2026-10-08
**Context:** The node backend handles voice turns. Vesper's long-term memory (Stage 1 brain: `~/MEMORY.md`, `~/memory/*.md`, curated via memory tools) is personal and curated. The node backend must NEVER write to it directly.

## Principle

**The backend proposes, Vesper disposes.** The node backend stages memory *candidates* in a queue. Vesper's main agent reviews, dedups, and curates them into the real memory on her normal cadence. No direct writes to curated memory from the node pipeline — ever.

## 1. Significance detection (node backend)

What counts as "significant" enough to propose for long-term memory:

- **Explicit (high confidence):** user says "remember this", "don't forget", "save that", "note that".
- **Heuristic (medium confidence):** the turn contains a durable fact, preference, commitment, or event. Examples: "I like my coffee black", "Charity's birthday is next week", "remind me to call the plumber".
- **Excluded:** small talk, follow-up questions ("tell me more"), transient state ("what's the weather"), anything ambiguous.

Start CONSERVATIVE: explicit triggers + a tight set of high-confidence patterns. A missed memory is a minor gap; a wrong memory pollutes the curated store. Expand the heuristic only after the explicit path proves out.

Implementation note: significance is judged in the node backend (it owns the transcript). A lightweight classifier or a dedicated brain `/ask` ("is this worth remembering?") — implementer's call, but it must be cheap and must not add user-facing latency (run it async, after the turn completes).

## 2. Candidate queue

- **Location:** `~/memory/node-candidates.jsonl` on the Studio (append-only, 600 permissions, never leaves the Studio).
- **Format (one JSON object per line):**
  ```json
  {
    "ts": "2026-10-08T15:30:00-05:00",
    "node_id": "homelink-c86320",
    "room": "office",
    "user_text": "remember that I like my coffee black",
    "vesper_reply": "Got it — black coffee, noted.",
    "significance": "explicit",
    "reason": "user said 'remember that'",
    "status": "pending"
  }
  ```
- **Never stored:** audio bytes, full transcripts (candidate carries a summary or the single significant exchange only), anything not clearly Kevin's own statement.
- **Attribution:** candidates carry `room` so Vesper can write "Kevin said in the office…" when the location matters, or drop it when it doesn't.

## 3. Review and incorporation (Vesper's main agent)

- Vesper reviews the queue on her normal cadence (e.g., during daily review, or when Kevin asks "anything new?").
- For each candidate she: (a) checks it against existing memory via `muse.memory_search` (dedup), (b) decides if it's actually durable, (c) writes it to the right place (`MEMORY.md` for standing facts, daily notes for events), (d) marks the candidate `reviewed` (or moves it to a processed file).
- Rejected candidates are marked `rejected` with a one-line reason — never silently dropped, so the pattern can be tuned.

## 4. Dedup

- Backend-side: include a `dedupe_key` (hash of the normalized core fact) so exact repeats are trivially collapsible.
- Vesper-side: `muse.memory_search` before writing; merge with existing entries rather than duplicating.
- If a candidate contradicts existing memory, Vesper flags it to Kevin rather than overwriting — memory conflicts are his call.

## 5. Privacy and boundaries

- Eligible: Kevin's own statements, preferences, household facts, commitments he makes via the node.
- Never: raw audio, full turn transcripts, anything said by another person in the room (the node hears the household — candidates must be attributable to Kevin or clearly marked).
- The queue file is Studio-only, 600. Candidates are Kevin's data; they follow the same discretion rules as all his memory.

## 6. Lifecycle and retention

- `pending` → `reviewed`/`rejected` via Vesper's review.
- Reviewed candidates are archived (not deleted) for 90 days, then pruned — the curated memory is the system of record, not the queue.
- If the queue grows past a sane bound (e.g., 500 pending), the backend logs a warning — a stuck review loop is a bug.

## Open questions for Kevin

1. **Aggressiveness:** start with explicit-only ("remember this"), or include the heuristic from day one? (Recommendation: explicit-only first.)
2. **Room attribution:** "Kevin said in the office…" vs. plain "Kevin said…"? (Recommendation: include room when it's a household/location fact, drop it otherwise.)
3. **Cross-node:** when there are multiple nodes, should a memory from the office node be distinguishable from the bedroom node? (Recommendation: yes, via the existing `node_id`/`room` fields — already in the candidate format.)
