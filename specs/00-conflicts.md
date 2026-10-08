# Conflicts / tensions in the source

Only one doc was ingested (`docs/vesper-node-architecture.md`). These tensions are internal to that doc. None of them is resolved here. Kevin decides.

## C1 — Who defines the node↔backend protocol, and when?
- §2: "**Our protocol (to be spec'd in the firmware task):** HTTPS note upload … reply as SSE/chunked stream carrying text deltas **plus a per-message TTS MP3 URL**"
- §7 B1: "… → TTS reply … → return audio (capability URL **or chunked**)."
- §7 Suggested order: "B3+B1+B2 (backend against a stubbed client) → F1/F2/F3 (firmware, needs B1 live)"

The protocol is assigned to the firmware task (F1), but the backend (B1) is built first. B1's reply shape ("capability URL or chunked") is also looser than §2's (SSE/chunked text deltas + per-message MP3 URL). **Handling in tasks:** task 07 (B1) must write the wire protocol doc before it builds anything, using §2's shape as the default. Task 09 (F1) consumes that doc and may amend it.

## C2 — Single shared node token vs per-node credentials
- §4 Edge: matcher `@node_auth header Authorization "Bearer {env.VESPER_NODE_TOKEN}"` (one static token checked at the edge).
- §4 Claim flow: "backend issues the node credential → stored in NVS `muse` namespace". §7 B2: "credential issuance for the claim flow".
- §6: "per-node token **or** one `VESPER_NODE_TOKEN` + node_id claim".

If each node gets its own token, a Caddy matcher on one `{env.VESPER_NODE_TOKEN}` can't authorize it. That works only if the edge token is shared and the per-node credential sits alongside it. **Handling in tasks:** task 08 (B2) records the chosen model as a decision. Task 12 (B4) must match it.

## C3 — Cross-reference error
- §3: "**Voice choice is Kevin's open decision** (see §7)". The open questions are actually in §8, and §7 is the work breakdown. This is minor and has no design impact.

## C4 — "First voice round trip verified" vs "stock firmware cannot speak replies"
- Header: "Flash day (stock firmware) completed 2026-10-07: paired as `MuseGadget-C86320`, first voice round trip verified."
- §1: "the Oct 7 'loud and clear' round trip therefore could not have included spoken reply audio — it was captions + a working mic path."

The doc flags this itself as open question 7. It affects what F2 (task 10) must deliver.
