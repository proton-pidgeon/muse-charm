# Task 12: Peggy static handle `/vesper-node/*` (B4) — HUMAN-GATED

**Goal:** `https://peggy.fly.dev/vesper-node/*` forwards to the node backend only with a valid `VESPER_NODE_TOKEN` bearer and fails closed at the edge otherwise.
**Depends on:** 07 (and the 08 credential decision)
**Relevant specs:** [`specs/01-vesper-node-architecture.md`](../specs/01-vesper-node-architecture.md), [`specs/00-conflicts.md`](../specs/00-conflicts.md) (C2)
**Source:** `docs/vesper-node-architecture.md` §4, §7 B4
**Est. effort:** not stated in source

## Deliverables
- [ ] Caddyfile block in `~/code-local/peggy/.peggy/render/Caddyfile` mirroring `/vesper/*` (lines ~302–355): `handle /vesper-node/*`, matcher `@node_auth header Authorization "Bearer {env.VESPER_NODE_TOKEN}"` → `uri strip_prefix /vesper-node` → `reverse_proxy http://[6PN]:<port>` + `header_up X-Forwarded-Proto https`; else `respond "bad token" 401`
- [ ] Token ≥32 chars, set as a Fly secret via `fly secrets import --stage`
- [ ] **Kevin's explicit approval obtained** before `fly deploy` from `.peggy/render/` (shared gateway; redeploys blip all services)
- [ ] Firmware `host` pointed at the Peggy URL

## Definition of done
- [ ] No token / wrong token → 401 from the edge; the backend logs show no request
- [ ] Correct token → turn succeeds via `peggy.fly.dev/vesper-node/...`
- [ ] The node token cannot reach `/vesper/*` master routes
- [ ] Other Peggy services healthy after deploy

## Anti-deliverables (do NOT build in this task)
- Inventing a new auth scheme ("Follow the `/vesper/` bearer-token precedent exactly")
- Deploying without Kevin's approval

## Risks / unknowns
- Conflict C2: a single edge token vs per-node credentials
- A redeploy blips every Peggy service

## Notes for Claude Code
- Start by reading the linked `specs/` files and, if needed, the original source doc.
- Tick off boxes in this file as you complete them.
- If a deliverable is blocked, append `> blocked: <reason>` under it rather than removing it.
- Commit incrementally; one logical change per commit.
