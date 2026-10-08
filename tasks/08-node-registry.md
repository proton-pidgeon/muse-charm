# Task 08: Node registry + credential issuance (B2)

**Goal:** The node backend maps `node_id` → room and issues node credentials through a claim endpoint, and turns carry room context.
**Depends on:** 07
**Relevant specs:** [`specs/01-vesper-node-architecture.md`](../specs/01-vesper-node-architecture.md), [`specs/00-conflicts.md`](../specs/00-conflicts.md) (C2)
**Source:** `docs/vesper-node-architecture.md` §4, §6, §7 B2
**Est. effort:** not stated in source

## Deliverables
- [x] Registry: `node_id` (`homelink-<mac>`) → room (`"kitchen"`, `"office"`, …) as backend config ("file or tiny admin endpoint")
- [x] **Decision recorded** on the credential model (conflict C2): per-node token vs one `VESPER_NODE_TOKEN` + node_id claim
  > decided: shared edge bearer `VESPER_NODE_TOKEN` (unchanged for task 12) **plus** a per-node `X-Node-Credential` issued at claim time, hashed in the registry, revocable per node; see `docs/node-wire-protocol.md` *Credential model: decision*
- [x] Claim endpoint(s): node boots unclaimed → shows a short claim code → Kevin (or Vesper) claims it against the backend → backend issues the node credential
- [x] Turn handler looks up the room and prepends room context to the turn text; `node_id` passed as `/ask` `device_id`

## Definition of done
- [x] Tests: claim → credential issued → authenticated turn carries the right room context; an unclaimed or unknown node is rejected
- [x] Credential storage at 600 perms; no credential material in git or logs

## Anti-deliverables (do NOT build in this task)
- Firmware claim UI (task 11)
- Peggy deploy (task 12)

## Risks / unknowns
- Open Q5: room names for nodes 2 and 3 are unknown

## Notes for Claude Code
- Start by reading the linked `specs/` files and, if needed, the original source doc.
- Tick off boxes in this file as you complete them.
- If a deliverable is blocked, append `> blocked: <reason>` under it rather than removing it.
- Commit incrementally; one logical change per commit.
