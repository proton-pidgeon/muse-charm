# Task 11: Claim flow firmware (F3)

**Goal:** A fresh node boots unclaimed, shows a claim code (screen + BLE), receives a backend-issued credential, and stores it in NVS `muse`.
**Depends on:** 08, 09
**Relevant specs:** [`specs/01-vesper-node-architecture.md`](../specs/01-vesper-node-architecture.md), [`specs/00-conflicts.md`](../specs/00-conflicts.md) (C2)
**Source:** `docs/vesper-node-architecture.md` §1, §4, §7 F3
**Est. effort:** not stated in source

## Deliverables
- [ ] Replace `link_pairing.c` with the claim flow: unclaimed boot → short claim code on screen and over BLE → backend-issued credential
- [ ] Store the credential under a new key in NVS `muse` (keep the existing `muse` keys as-is)
- [ ] Evaluate NVS encryption (currently plaintext, `config_store.c:39-45`) and record the decision
- [ ] Keep `muse_ble.c` Wi-Fi provisioning commands (`wifi.ssid`/`wifi.pass`/`wifi.connect`, :123-175)
- [ ] Device ID from `identity_node_id()` → `homelink-<mac>`
- [ ] Refresh-on-401 pattern imitated from `app.c:860-935` if the credential model has refresh

## Definition of done
- [ ] Fresh flash → claim code shown → claimed via backend → authenticated turns work after reboot
- [ ] Credential never printed to serial logs

## Anti-deliverables (do NOT build in this task)
- Muse-app BLE pairing / any Meta endpoint
- OTA (task 13)

## Risks / unknowns
- HUMAN GATE: claim ceremony performed by Kevin (or Vesper)

## Notes for Claude Code
- Start by reading the linked `specs/` files and, if needed, the original source doc.
- Tick off boxes in this file as you complete them.
- If a deliverable is blocked, append `> blocked: <reason>` under it rather than removing it.
- Commit incrementally; one logical change per commit.
