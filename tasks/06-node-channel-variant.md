# Task 06: `channel="node"` persona variant in vesper-voice (B3)

**Goal:** `vesper-voice` supports a `channel="node"` persona/spoken-format variant that a node turn can request through `/ask`.
**Depends on:** none
**Relevant specs:** [`specs/01-vesper-node-architecture.md`](../specs/01-vesper-node-architecture.md)
**Source:** `docs/vesper-node-architecture.md` §3, §6, §7 B3
**Est. effort:** not stated in source

## Deliverables
- [ ] `channel="node"` variant in `persona.py::system_prompt(channel)` alongside `ask`, `voice`, `phone`
- [ ] `to_spoken` caps for `node` (existing channels use 2–3 sentence caps)
- [ ] A way for a `/ask` caller to request the `node` channel ("`POST /ask {text, device_id: node_id}` (+ `channel="node"`)")
- [ ] Room context support: the backend "can prepend room context to the turn text". Make sure the node prompt variant accommodates this
- [ ] Tests in the existing `vesper-voice` test suite

## Definition of done
- [ ] `vesper-voice` test suite green, with new tests covering the `node` channel
- [ ] Existing `ask`/`voice`/`phone` channel behavior unchanged (their tests still pass)
- [ ] Existing `/ask` contract preserved: auth-before-body, constant-time compare, 25 s deadline, bounded tool loop (max 6 rounds, 4 tool calls/round)

## Anti-deliverables (do NOT build in this task)
- Forking `vesper-voice` ("small change in the `vesper-voice` repo, not a fork")
- New agent/tool code. Lobe tools (search/get/toggle/set) are already in the `/ask` loop

## Risks / unknowns
- Open Q3 (v1 capability allowlist: are purchases / messaging people / irreversible actions in or out?) may shape the node prompt. Confirm with Kevin before deploying
- Work happens in `~/vesper-voice` (live launchd `com.vesper.brain`), not this repo. Restarting it affects the live brain

## Notes for Claude Code
- Start by reading the linked `specs/` files and, if needed, the original source doc.
- Tick off boxes in this file as you complete them.
- If a deliverable is blocked, append `> blocked: <reason>` under it rather than removing it.
- Commit incrementally; one logical change per commit.
