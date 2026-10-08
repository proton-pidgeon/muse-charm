# Task 11: Claim flow firmware (F3)

**Goal:** A fresh node boots unclaimed, shows a claim code (screen + BLE), receives a backend-issued credential, and stores it in NVS `muse`.
**Depends on:** 08, 09
**Relevant specs:** [`specs/01-vesper-node-architecture.md`](../specs/01-vesper-node-architecture.md), [`specs/00-conflicts.md`](../specs/00-conflicts.md) (C2)
**Source:** `docs/vesper-node-architecture.md` §1, §4, §7 F3
**Est. effort:** not stated in source

## Deliverables
- [x] Replace `link_pairing.c` with the claim flow: unclaimed boot → short claim code on screen and over BLE → backend-issued credential
  - `firmware/hatch/vesper_claim.{c,h}` is the pure-C core, host-tested in `hatch/test/test_vesper_claim.c`, under the same `-Werror` + ASan/UBSan flags. `muse_chat_vesper.c` runs it on the hatch task between turns:
    - `POST /claim/start` with the bearer and `X-Node-Id`;
    - the code goes on the idle caption, the settings status and the BLE claim characteristic, never on serial;
    - the secret is kept in RAM only;
    - `POST /claim/poll` every 3 s (`202` keep polling, `404` start over, `429`/`Retry-After` honoured);
    - on `200`, the credential is stored.
  - `hatch/vesper_ble.c` is the node's BLE host (the task-09 build had none). It carries `muse_ble.c` unchanged plus the claim service. No Meta pairing or endpoint.
  - The SDK side is the new `sdk-patches/0004`. `make -C firmware live-claim` ran the whole ceremony against a throwaway backend through this code.
- [x] Store the credential under a new key in NVS `muse` (keep the existing `muse` keys as-is)
  - The new key is `muse:node_cred` (`hatch/vesper_cred.c`), written from an internal-RAM-stack helper task. No other `muse` key is touched. The setup reset and serial `>claim.forget` erase it.
- [x] Evaluate NVS encryption (currently plaintext, `config_store.c:39-45`) and record the decision
  - **Decision: plaintext on the dev boards; no eFuse burned, flash encryption off.**
    - `CONFIG_HOMEHUB_NVS_ENCRYPTION` (HMAC NVS encryption) burns an HMAC key into an S3 eFuse block on first boot, which is irreversible.
    - Without secure boot and flash encryption it only stops a raw flash dump: anyone who can flash code can use the same HMAC peripheral to decrypt.
    - The real defence (secure boot v2 + release-mode flash encryption + disabled USB download) is a set of one-way eFuse steps that end the bench workflow.
    - The credential only works with the shared bearer, which sits in the same plaintext NVS, and only as that one node. It can be revoked alone (`vesper-node nodes revoke`).
    - Revisit with production signing and OTA (task 13).
  - The full rationale is in `firmware/README.md`, *NVS encryption: decision*.
- [x] Keep `muse_ble.c` Wi-Fi provisioning commands (`wifi.ssid`/`wifi.pass`/`wifi.connect`, :123-175)
  - `muse_ble.c` is byte-identical to `b1a3822` and is served again by the new BLE host (passkey pairing as stock). The same commands also work over the serial console, as before.
- [x] Device ID from `identity_node_id()` → `homelink-<mac>`
  - `X-Node-Id` on every claim, turn and audio request is `identity_node_id()`. `identity.c` is byte-identical.
- [x] Refresh-on-401 pattern imitated from `app.c:860-935` if the credential model has refresh
  - **Resolved: no refresh in this credential model.** Task 08 issues no refresh token and the credential doesn't expire. The equivalent is **re-claim on `403 node_unauthorized`** on `/turn` (`vc_needs_claim` → `vc_reclaim`). The old credential stays in NVS until a new one replaces it. A `401` is a wrong shared bearer, which no refresh could fix, so it stays `TOKEN REFUSED`.

## Definition of done
- [ ] Fresh flash → claim code shown → claimed via backend → authenticated turns work after reboot
> blocked: HUMAN GATE. The agent must not flash the board or open its serial port. The host side is proven: `make -C firmware live-claim` ran the whole flow against a throwaway task-08 backend through the firmware's own claim code. The steps were claim/start, code, CLI approval, poll `200`, credential stored, then a "reboot": `/turn` and `/audio` accepted the stored credential and refused its absence with `403 node_unauthorized`. A fresh `b1a3822` tree with `apply-sdk.sh` (run twice) builds clean. Steps for Kevin (also in `firmware/README.md`, *On-device check (task 11)*):
> 1. Restart `com.vesper.node` so it runs the task-08 code: `cd ~/builds/muse-charm/muse-charm && git pull && make -C backend install && launchctl kickstart -k gui/$(id -u)/com.vesper.node`. Check with `make -C backend check-config` and `curl -s http://[::1]:8796/healthz`. Keep the board's LAN route to it (the socat forward on `:8797`) until task 12.
> 2. Build and flash: `firmware/apply-sdk.sh ~/builds/muse-charm/scratch/sdk-impl-11 && cd ~/builds/muse-charm/scratch/sdk-impl-11/esp32 && tools/muse/board.sh build aipi && tools/muse/board.sh flash aipi /dev/cu.usbmodem83201`. Then start the monitor: `. ~/esp/esp-idf-v6.0.1/export.sh && idf.py -B build-muse-aipi -p /dev/cu.usbmodem83201 monitor | tee /tmp/claim-serial.log`. The bench board has no `node_cred`, so it boots unclaimed. For a truly fresh board, `erase-flash` first and provision Wi-Fi, `hatch.host` and `hatch.token` as in task 09.
> 3. The claim code shows on screen as `CLAIM CODE XXXX-XXXX`. The serial log shows `claim: code 1 ready` and `vesper_ble: advertising as MuseGadget-XXXXXX (claim in progress)`, but never the code itself. Optional: in nRF Connect, service `76657370-…-0001` reads `{"state":"pending","code":"XXXX-XXXX"}`.
> 4. Approve the code: `cd ~/builds/muse-charm/muse-charm/backend && uv run --locked vesper-node claim XXXX-XXXX --room <room>`. Within about 3 s, serial shows `claim: claimed, room <room>; credential saved in NVS` and the screen shows `CLAIMED - <room>`. `vesper-node nodes list` shows `access=credential`.
> 5. Reboot the board. Serial should show `node credential: stored` and no `claim:` lines.
> 6. Push-to-talk "what is two plus two". Expect `turn: HTTP 200` and the spoken reply. The backend log should show `turn done: node=homelink-c86320 room=<room> auth=credential ok=True`.
> 7. Run `grep -cE 'v(nc|cs)_' /tmp/claim-serial.log`. It should print `0`.
- [x] Credential never printed to serial logs
  - Host evidence:
    - Every `make -C firmware test` runs `hatch/test/check_log_hygiene.py`. It parses every logging call in `hatch/*.c|h` and in every line the patch set adds, and fails on any argument holding the credential, the claim secret, the bearer, the `Authorization` buffer, a header value or the claim code. It is self-tested against known-bad calls; the current result is 16 files, 0 offending calls.
    - `make -C firmware live-claim` greps the node output and the backend log for the credential, the secret, the token and any `vnc_`/`vcs_` shape. It finds none.
  - `>status` reports `"credential":true|false` only. On the device, step 7 above confirms it.

## Anti-deliverables (do NOT build in this task)
- Muse-app BLE pairing / any Meta endpoint
- OTA (task 13)

## Risks / unknowns
- HUMAN GATE: claim ceremony performed by Kevin (or Vesper)
- First on-device run of the restored BLE host. NimBLE starts lazily from the keeper task once Wi-Fi is up, as the stock server did. Watch the heartbeat's internal-RAM figure after `BLE host started`.

## Notes for Claude Code
- Start by reading the linked `specs/` files and, if needed, the original source doc.
- Tick off boxes in this file as you complete them.
- If a deliverable is blocked, append `> blocked: <reason>` under it rather than removing it.
- Commit incrementally; one logical change per commit.
