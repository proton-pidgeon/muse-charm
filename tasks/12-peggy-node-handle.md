# Task 12: Peggy static handle `/vesper-node/*` (B4) — HUMAN-GATED

**Goal:** `https://peggy.fly.dev/vesper-node/*` forwards to the node backend only with a valid `VESPER_NODE_TOKEN` bearer and fails closed at the edge otherwise.
**Depends on:** 07 (and the 08 credential decision)
**Relevant specs:** [`specs/01-vesper-node-architecture.md`](../specs/01-vesper-node-architecture.md), [`specs/00-conflicts.md`](../specs/00-conflicts.md) (C2)
**Source:** `docs/vesper-node-architecture.md` §4, §7 B4
**Est. effort:** not stated in source

## Deliverables
- [x] Caddyfile block in `~/code-local/peggy/.peggy/render/Caddyfile` mirroring `/vesper/*` (lines ~302–355): `handle /vesper-node/*`, matcher `@node_auth header Authorization "Bearer {env.VESPER_NODE_TOKEN}"` → `uri strip_prefix /vesper-node` → `reverse_proxy http://[6PN]:<port>` + `header_up X-Forwarded-Proto https`; else `respond "bad token" 401`
  > done: peggy `daa1016` + `8530178` (`examples/Caddyfile`, rendered via `scripts/render.sh`). Matcher named `@vesper_node_auth` (sibling naming) and AND'd with `expression {env.VESPER_NODE_TOKEN}.size() >= 32` so an unset/empty secret fails closed (review gate: GPT-5 HIGH -> Fable non-blocking after the guard).
- [x] Token ≥32 chars, set as a Fly secret via `fly secrets import --stage`
  > done: the backend's own 48-char `VESPER_NODE_TOKEN` (distinct from the brain/phone tokens), staged 2026-10-08 and deployed.
- [x] **Kevin's explicit approval obtained** before `fly deploy` from `.peggy/render/` (shared gateway; redeploys blip all services)
  > approved 2026-10-08 (HANDOVER); deployed 2026-10-08 17:02 UTC, machine 83d154ec7572e8 healthy.
- [ ] Firmware `host` pointed at the Peggy URL
  > blocked: `hatch.host` is serial-provisioned NVS, needs Kevin at the board: `>hatch.host=https://peggy.fly.dev/vesper-node` then `>hatch.test`. `firmware/README.md` now points there by default.

## Definition of done
- [x] No token / wrong token → 401 from the edge; the backend logs show no request
  > verified: no/wrong/empty-bearer on `/healthz` and `/turn` -> `bad token` 401, 0 backend log lines.
- [x] Correct token → turn succeeds via `peggy.fly.dev/vesper-node/...`
  > verified: `stub_client.py --url https://peggy.fly.dev/vesper-node` with a temporary shared-token node (`homelink-peggytest`, removed after): transcript -> "Four." -> MP3 fetched through the edge, 2.0 s client total. On-device turn via Peggy still needs the board re-provisioned (above).
- [x] The node token cannot reach `/vesper/*` master routes
  > verified: node bearer on `/vesper/healthz` and `/vesper/ask` -> 401; brain token still 200.
- [x] Other Peggy services healthy after deploy
  > verified: 15-route status sweep identical to the pre-deploy baseline except `/vesper-node/healthz` 404 -> 401; all 20 registrar services re-registered within ~1 min (heartbeat self-heal).

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
