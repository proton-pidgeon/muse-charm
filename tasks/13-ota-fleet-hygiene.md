# Task 13: OTA + fleet hygiene (F4)

**Goal:** `ota.c` updates nodes from our server so a multi-room fleet can be updated without USB.
**Depends on:** 09, 12
**Relevant specs:** [`specs/01-vesper-node-architecture.md`](../specs/01-vesper-node-architecture.md)
**Source:** `docs/vesper-node-architecture.md` §6, §7 F4
**Est. effort:** not stated in source

## Deliverables
- [ ] Keep `ota.c` working against our server (update source on the node backend / Peggy)
- [ ] Fleet notes: per-node firmware identical except NVS identity; N nodes = N registry rows, N claim ceremonies, one backend

## Definition of done
- [ ] An OTA update served by our backend is applied on the device and the node comes back claimed and working

## Anti-deliverables (do NOT build in this task)
- Multi-room expansion itself (comes after this in the suggested order)

## Risks / unknowns
- HUMAN GATE: device in hand for the first OTA test

## Notes for Claude Code
- Start by reading the linked `specs/` files and, if needed, the original source doc.
- Tick off boxes in this file as you complete them.
- If a deliverable is blocked, append `> blocked: <reason>` under it rather than removing it.
- Commit incrementally; one logical change per commit.
