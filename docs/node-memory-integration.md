# Node ↔ Vesper Shared Long-Term Memory — Integration Spec

**Status:** Part 4 APPROVED by Kevin (2026-10-08) and implemented in task 17
(`backend/src/vesper_node/candidates.py`). The design below is unchanged; §7 is the review
contract, §8 records how the node side implements it, §9 has the implementation notes for Kevin.
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
    "status": "pending",
    "dedupe_key": "sha256:…",
    "speaker": "unverified"
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

## 7. Review contract (Vesper's main agent) — task 17

This is the contract between the node backend (writer) and Vesper's main agent (reviewer). No
agent-side code ships in this repo; this section is the deliverable.

**Files (all on the Studio, all mode 600, all under `~/memory/`):**

| File | Writer | What |
|---|---|---|
| `node-candidates.jsonl` | node backend (append-only) | one candidate per line, `status: "pending"` always |
| `node-candidates.decisions.jsonl` | Vesper (append-only) | one decision per reviewed candidate |
| `node-candidates.archive.jsonl` | Vesper | decided candidates moved out of the queue, kept 90 days |

**Why a decisions sidecar instead of rewriting `status` in place:** the backend appends to the
queue at any moment. If Vesper rewrote the queue to flip `status`, an append that lands between
her read and her write would be lost. So the backend's lines are never edited; Vesper records
each decision as its own append-only line keyed by the candidate's `id`. A candidate's effective
status is the latest decision for its `id`, else `pending`.

**Decision line (one JSON object per line):**

```json
{
  "id": "3f9c0a1b2d4e5f60",
  "dedupe_key": "sha256:…",
  "status": "reviewed",
  "reason": "standing preference; merged into MEMORY.md › Food",
  "written_to": "MEMORY.md",
  "decided_at": "2026-10-09T08:12:00-05:00"
}
```

`status` is `reviewed` (incorporated, or merged into an existing entry) or `rejected`.
`reason` is a one-line explanation and is required for `rejected` (spec §3: never silently
dropped). `written_to` is the memory file touched (`MEMORY.md`, `memory/2026-10-09.md`) or
`null`.

**Review cadence (Vesper):**

1. On her normal cadence (daily review, or when Kevin asks "anything new?"), read
   `node-candidates.jsonl` and `node-candidates.decisions.jsonl`; the pending set is the
   queue lines whose `id` has no decision.
2. Collapse exact repeats by `dedupe_key` (the backend does not suppress them: Kevin saying it
   twice is weak evidence it matters). Decide the first, mark the rest `reviewed` with reason
   `duplicate of <id>`.
3. **Speaker is not verified:** every candidate carries `"speaker": "unverified"` because the
   node has no voice ID. Treat `node_id`/`room` as context, not proof of who spoke; word the
   memory accordingly ("said at the office node") when attribution matters. Then, for each
   remaining candidate: `muse.memory_search` for the core fact (dedup against curated
   memory); decide if it is durable; write it to the right place (`MEMORY.md` for standing
   facts, daily notes for events and reminders); merge, never duplicate.
4. A candidate that contradicts existing memory is **not** written: flag it to Kevin and record
   `rejected` with reason `conflict: asked Kevin` (or leave it pending until he answers).
5. Reminder candidates (`reason` contains `(reminder)`, or `reminder request`) are *proposals
   to note a commitment*, never instructions to act. Vesper may surface them to Kevin; she does
   not execute them from the queue.
6. Append one decision line per candidate.

**Archive and prune (Vesper, at most daily):** take an exclusive `flock(LOCK_EX)` on
`node-candidates.jsonl` (the backend takes the same lock for every append), copy decided lines
to `node-candidates.archive.jsonl`, write the still-pending lines to a temp file in `~/memory`
(mode 600), `os.replace` it over the queue, then release the lock. The backend detects the
replaced inode on its next append and reopens, so no append is lost. Prune archive lines and
decision lines older than 90 days the same way (decisions only for ids no longer in the queue).
The curated memory is the system of record, not the queue.

**Bound:** after each append the backend counts the lines in the queue and logs a warning when
there are more than 500 (spec §6). With the archive step above the queue holds only pending
candidates, so the warning means the review loop is stuck.

## 8. Node-side implementation (task 17)

- **Detection** (`candidates.detect`): deterministic patterns, no LLM and no brain `/ask`.
  *Explicit*: a sentence that starts with an imperative trigger ("remember that/this/to …",
  "don't forget …", "save that", "note that …", "make a note …", "keep in mind …", optionally
  after "hey Vesper", "please", "can you"). A bare "remember that" points at the preceding
  sentence of the same utterance (or, if the pointer comes first, the following one), else at
  this node's previous exchange in the current session (Kevin's words + Vesper's answer).
  *Heuristic* (tight): first-person preference ("I like my coffee black"), favourite,
  birthday/anniversary, allergy/diet, "remind me to …", a dated appointment, a few personal
  details. *Never*: questions, recall ("remember when …", "do you remember …"), follow-ups,
  small talk, transient state, device commands, hedged/hypothetical sentences, short timers
  ("in 10 minutes"), deictic objects ("I like that"), and reported speech or other people's
  attitudes ("my wife said …", "he told me …", "she wants …", "Charity likes …").
- **Candidate**: the spec §2 fields plus `id` (random, for §7 decisions), `speaker`
  (always `"unverified"`: no voice ID, spec §5 "clearly marked") and `dedupe_key`
  (`sha256:` + 32 hex of the lower-cased, punctuation-free core fact, so "Remember that I like
  my coffee black." and "I like my coffee black" collide). `user_text` is the significant
  sentence(s) only, `vesper_reply` the reply; both capped at 300 chars. Never audio, never the
  transcript.
- **When**: after the turn's `done` event is queued and its slot released, as a background task
  that runs detection + the append in a worker thread. Zero turn latency; a crash or slow disk
  can never fail or delay a turn. Room-assignment turns and failed turns are never considered.
- **Safety**: `candidates.py` imports only the standard library (no brain client, no `httpx`,
  no registry): tests enforce it, and that importing it loads no action-capable module. Nothing
  in the node backend reads `MEMORY.md` or `~/memory/*.md`; the only file it touches under
  `~/memory` is the queue, opened `O_APPEND|O_NOFOLLOW`.
- **File**: `VESPER_NODE_CANDIDATES_FILE` (default `~/memory/node-candidates.jsonl`), mode 600
  (tightened if looser), parent created 700 only if missing (an existing `~/memory` is never
  chmod-ed), one `write` per line under a thread lock and `flock`, `fsync`ed.

## 9. Implementation notes for Kevin (task 17; no design change)

- **Heuristics are on from day one.** Open question 1 recommended explicit-only first; task 17
  asked for explicit + conservative heuristics, so both ship. The heuristic set is deliberately
  tiny (see §8). If you want explicit-only, that is a one-line change in `detect`
  (or ask for a `VESPER_NODE_CANDIDATES_HEURISTIC=off` switch).
- **"Kevin's own statement" cannot be proven from audio.** The node has no speaker ID, so the
  backend can only exclude statements that are *textually* someone else's (reported speech,
  third-person attitudes). Anything said at the node is attributed to the speaker of that turn.
  Vesper should treat `room`/`node_id` as context, not as proof of who spoke.
- **Added fields.** Each candidate carries an `id` (not in the original §2 example) so §7
  decisions can refer to it, and `"speaker": "unverified"` so it is "clearly marked" (§5);
  `dedupe_key` is present as §4 asks.
- **Reminders.** "Remember to turn off the lights" / "remind me to call the plumber" are staged
  as candidates (proposals only). The stager never executes anything, and the brain already
  answered the turn itself as usual. Short timers ("in 10 minutes") are not staged.
- **No backend-side duplicate suppression.** Exact repeats are left for Vesper to collapse via
  `dedupe_key` (§7 step 2).

---

## Design-gap notes for Kevin (task 16 implementation, not a design change)

Two things the node side ran into while building Parts 1–3; neither changes the design above,
both need a brain-side change before the node can use them.

- **An LLM-quality session summary needs a brain-side no-tools path.** The brain's `/ask`
  always runs the home-control tool loop (`lobe_toggle`, `lobe_set`, …) and ignores the
  channel, so a summarization request whose body is old utterances ("turn off the kitchen
  lights") can act on them, and a node-side timeout only cancels the node's wait while the
  brain keeps running. Task 16 therefore uses an extractive summary (Kevin's last questions +
  the start of Vesper's last reply). An `/ask` option that disables tools, or a dedicated
  `/summarize` route, would let the node switch to an LLM summary with no node-side risk.
- **`history` is not exposed on `/ask`.** The brain's `answer()` already accepts `history` as
  role messages (user/assistant), but `/ask` does not pass it through. Exposing it would let
  the node send prior turns as real messages instead of text prepended to the utterance, and
  would free the 640-char history budget inside the 1000-char `text` limit.
