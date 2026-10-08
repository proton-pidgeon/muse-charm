# firmware/

Vesper-specific firmware pieces on top of Meta's Muse gadget SDK at commit `b1a3822`. A
pristine clone lives at `~/builds/muse-charm/scratch/muse-gadget-sdk`. Every change to the SDK is
tracked here, and `firmware/apply-sdk.sh` turns a pristine checkout into the Vesper node firmware.

| Path | What |
|---|---|
| `apply-sdk.sh <SDK_DIR>` | Idempotent. Deletes the Meta transport, applies `sdk-patches/*.patch`, installs `hatch/` and the avatar, and checks that `main/voice.c` is unchanged |
| `sdk-patches/delete.txt` | Files the patch set removes from the SDK |
| `sdk-patches/000N-*.patch` | Edits to tracked SDK files (`git apply` format against `b1a3822`) |
| `hatch/` | The Vesper `muse_hatch_*` backend (task 09), its reply speech (task 10), the claim flow, node credential and BLE host (task 11) and the firmware update check (task 13), installed as `<SDK>/esp32/components/muse/vesper/` |
| `hatch/VERSION` | The firmware version (`MAJOR.MINOR.PATCH`) the build stamps into the image (task 13); bump it for every release |
| `hatch/test/` | Host tests of the protocol core, the claim flow, the speech helpers and the update check, the log-hygiene check, and the live-turn, live-claim and live-ota harnesses |
| `avatar/` | The Vesper owl avatar (task 04) |
| `Makefile` | `make -C firmware test`, `live-turn`, `live-claim` and `live-ota` (host only, no ESP-IDF) |

## Decision (task 09): a patch set tracked in THIS repo, not an SDK fork

The transport work follows the avatar's precedent. Nothing lives in a fork. The SDK checkout
stays a re-clonable upstream at `b1a3822`, and `apply-sdk.sh` rebuilds the Vesper tree from it:

1. **Deletions** come from a plain list (`sdk-patches/delete.txt`), not from diff hunks that
   would repeat ~10k lines of Meta source. A reviewer sees exactly which files go.
2. **Edits to tracked SDK files** are a short `git apply` series (`sdk-patches/0001-0003`):
   - build and Kconfig without `CONFIG_GADGET_SDK_TOKEN`;
   - `main/app.c` cut down to Wi-Fi, OTA validation, identity and the Muse glue;
   - the backend selection at `components/muse/CMakeLists.txt` (since task 10 it also builds
     `vesper/vesper_audio.c`: one line, the only SDK-side change task 10 needed);
   - the `muse_settings` repurpose;
   - since task 11, `0004`: the claim flow's sources in the build, the node's BLE host behind
     `app_ble_companion_set` and Link's `.ble_started`, the credential forgotten on a setup
     reset, and the serial console's `>status` `vesper` block and `>claim.forget` (see
     *Claim flow* below);
   - since task 13, `0005`: `ota.c` gains `ota_start_request` (the stock `ota_start` is
     unchanged), `app.c` registers it as the node's installer and keeps a fresh image only once
     the update server has answered, the console's `>ota.check`, `vesper/vesper_ota.c` in the
     build, and `PROJECT_VER` from `hatch/VERSION` (see *Firmware updates* below).

   `0004` edits lines that `0002` added (in `app.c` and `muse_glue.c`), so `apply-sdk.sh`
   can't ask "is `0002` applied?" of `0002` alone once `0004` is on top. It first asks it of the
   whole series: it reverse-applies the patches, last to first, to a scratch copy of the files
   they touch. If that works, the tree is fully patched. Otherwise it applies patch by patch as
   before. Run it twice and the second run changes nothing.
3. **New Vesper-owned sources** are whole files in `hatch/`, copied into an SDK path that the
   SDK's `.gitignore` already ignores (`components/muse/vesper/`, like `components/muse/avatar/`).

Why not a fork: every change shows up in this repo's diff, where the review gate looks. The patch
set re-applies to a fresh `b1a3822` clone in one command. Upstream drift stays visible: a patch
that stops applying fails loudly instead of being merged silently. If the SDK ever needs
upstream updates, rebase the patches.

Reproduce from scratch:

```sh
git -C ~/builds/muse-charm/scratch/muse-gadget-sdk worktree add --detach /tmp/sdk-vesper b1a3822
firmware/apply-sdk.sh /tmp/sdk-vesper
cd /tmp/sdk-vesper/esp32 && tools/muse/board.sh build aipi     # ESP-IDF v6.0.1, see docs/aipi-lite-bringup.md
```

### What the patch set removes (Meta transport)

- `main/vm_api.c`
- `main/link_pairing.c`
- `main/noise_control*`
- `main/ble_server.c`
- `components/muse/muse_account_api.c`
- `components/muse/muse_chat_session.cpp`
- `CONFIG_GADGET_SDK_TOKEN`, from Kconfig and `validate_config.cmake`

Also removed, because only that transport used it: the Noise tunnel and tunnel netif,
`net_discovery`, the pairing signer/transcript, `factory_test` (BLE), `bug_report` and
`image_fetch` (remote-control commands), `muse_chat_link.c` (the no-PSRAM backend over Link's
session), and the `noise_core` component.

The image was 1,839,104 bytes after task 09, down from 2,166,784 for the avatar-only stock
build. With task 11's BLE host (NimBLE is linked and started again) it is 2,035,712 bytes
(`0x1f1000`, 51% of the app partition free). Task 13's update check keeps it at 2,035,712
bytes signed (2,031,616 unsigned; the signed image is padded to the signature block), so an
update always fits the other 4 MiB slot.

**Kept byte-identical:**

- `main/voice.c` (the Voice PE voice task; not even compiled for the AIPI)
- `board_aipi.c`, `voice_board.c`, `voice_player.c`
- the LVGL UI (`muse_ui.c`, `muse_settings_ui.c`, `muse_menu.c`)
- `muse_voice.c`, the AIPI's push-to-talk task, which talks to the new backend through the
  unchanged `muse_hatch_*` API
- `muse_wifi`, `identity.c`, `muse_ble.c`
- `ota.c`'s stock path (`ota_start`, the version gate, the download task) is unchanged; task 13
  adds `ota_start_request` beside it (patch `0005`) rather than writing a second OTA client

`apply-sdk.sh` fails if `voice.c` changes.

Task 11 kept that list as it was. `muse_ble.c` in particular is unchanged: the node's new BLE
host (`hatch/vesper_ble.c`) registers its service and hooks exactly as `ble_server.c` did. The
one stock file task 11 edits that wasn't already patched is `components/muse/muse_input.c`
(the serial console, not on the list): `>status` gains a `vesper` block (presence only) and
`>claim.forget` is new. The kept-identical `muse_ble.c` builds the `>status` `device` block,
so the credential's presence had to go beside it rather than into it.

**The one `GADGET_SDK_TOKEN` residue:** `identity.c` must stay byte-identical, and its
`identity_sdk_token()` still names the macro. Nothing calls that function any more, so
`main/CMakeLists.txt` gives that one file `CONFIG_GADGET_SDK_TOKEN=""`. The Kconfig option, the
validation and every use are gone. The only place the name still appears in the build is that
compile definition.

**Supported boards:** Muse boards with PSRAM only (`CONFIG_MUSE_HATCH`, which includes the AIPI).
Link-only and Voice PE builds depended on the deleted transport. `components/muse/CMakeLists.txt`
now stops them with a clear `FATAL_ERROR`. The SDK's own pytest suite under `esp32/tests/` covers
upstream Home Link code, and many of its tests target deleted files. It is not part of this
patch set's checks.

### The backend (`hatch/`)

- **`vesper_proto.{c,h}`** is the protocol core, pure C with no ESP-IDF dependencies. It covers:
  - request headers, with header-injection-safe token and node-id checks;
  - base URL parsing;
  - `audio_url` resolution, relative references only, so the bearer never leaves the
    configured server;
  - bounded SSE framing (comments skipped unbuffered, splits anywhere, CRLF/LF/CR);
  - flat-JSON field decoding (escapes, surrogate pairs);
  - UTF-8-safe truncation;
  - HTTP status verdicts.

  The firmware and the host tests compile the same file.
- **`vesper_audio.{c,h}`** (task 10) is the reply-speech arithmetic, pure C as well: the
  22.05 kHz -> 16 kHz resampler, the MP3 buffer accounting of the decode path, and the caption
  timing numbers. See *Speech* below.
- **`muse_chat_vesper.c`** is the ESP-IDF backend behind the `muse_hatch_*` seam
  (`turn_begin/audio/end/cancel/event/read/caption`, plus status, test and the MP3 self-test). It
  uses `esp_http_client`:
  - It opens a chunked POST at the press and streams the note while the button is held.
  - It parses the SSE reply on the hatch task.
  - Text deltas become captions.
  - `message_done.audio_url` goes to `vesper_tts_slot_offer()`, **the TTS slot**, which task 10
    filled: the reply is spoken (see *Speech* below). Task 09 shipped a weak default that declined.

  Logs carry lengths, statuses and timings only. They never carry the token, transcript or reply
  text.
- **`vesper_claim.{c,h}`** (task 11) is the node side of the claim flow, pure C like
  `vesper_proto.c`: claim header building and validation (credential, claim secret, code, room;
  all header-injection-safe), `/claim/start` and `/claim/poll` response parsing, and the claim
  state machine with its poll, backoff and `Retry-After` verdicts. See *Claim flow* below.
- **`vesper_cred.{c,h}`** (task 11) keeps the node credential in NVS `muse:node_cred` and in RAM.
- **`vesper_ble.{c,h}`** (task 11) is the node's BLE host: Muse's setup service (`muse_ble.c`,
  unchanged) plus the Vesper claim service.
- **Wire protocol:** `docs/node-wire-protocol.md` v1. Task 09 added the *Node firmware notes*
  section; the wire shape is unchanged.

### Configuration and provisioning a dev board

NVS namespace `muse`:

- **`host`** is repurposed as the server **base URL**, at most 63 characters. Turns go to
  `<host>/turn`.
- **`node_token`** is a new key for the node bearer. The stock `token` key may hold a Meta device
  token and is never read.
- **`node_cred`** (task 11) is a new key for the per-node credential (`vnc_…`). The node gets it
  from the claim flow; it is never typed in. The other `muse` keys are left as they are.

`host` and `node_token` are empty until provisioned. The Kconfig fallbacks
`CONFIG_VESPER_NODE_URL` and `CONFIG_VESPER_NODE_TOKEN` default to `""`. Never commit a token in
them. NVS stays plaintext: see *NVS encryption: decision* below.

Wi-Fi and the backend are provisioned over **Muse's USB serial console**, or over BLE with Muse's
phone-setup service (the same `key=value` commands; task 11 brought the BLE host back, see
*Claim flow*). Every serial line starts with `>`. `wifi.pass` and `hatch.token` are never
echoed.

```
>wifi.ssid=<network>
>wifi.pass=<password>
>wifi.connect
>hatch.host=https://peggy.fly.dev/vesper-node      (or a LAN URL, see the human gate)
>hatch.token=<VESPER_NODE_TOKEN>                    (from ~/.config/vesper-voice/node.env)
>hatch.test                                         (GET <host>/healthz with the bearer)
>status                                             (JSON; "vesper":{"credential":true,"claim":"claimed"},
                                                     "hatch":{"token":true,...}: presence only)
>claim.forget                                       (forget the node credential and claim again)
```

You can also join Wi-Fi from Muse's on-screen Wi-Fi settings, or with the
`CONFIG_HOMEHUB_WIFI_SSID` dev override. A setup reset from the Muse menu forgets the saved Wi-Fi,
the node token and the node credential, and keeps the server URL. After the restart the node
claims again.

### Claim flow (task 11, F3)

`link_pairing.c` (the Muse app's BLE pairing) is gone (task 09). In its place the node claims
itself from the Vesper node backend (`docs/node-wire-protocol.md`, *Claim flow*). The credential
model is task 08's (conflict C2): the shared edge bearer plus a per-node `X-Node-Credential`.

- **When.** At boot the node loads `muse:node_cred`. With no credential (a fresh flash, a setup
  reset, `>claim.forget`) it claims as soon as the server URL and token are set and Wi-Fi is up.
  A `403 node_unauthorized` on `/turn` or on an `/audio` GET (that clip is then paced silently) sends a claimed node back to the claim flow too. The stored
  credential stays in NVS until a new one replaces it, so a refusal that was only a blip costs a
  re-approval, never the node's identity.
- **Start.** `POST <host>/claim/start` with the bearer and `X-Node-Id: homelink-<mac>`
  (`identity_node_id()`; `identity.c` is unchanged).
- **The code.** The `claim_code` (`XXXX-XXXX`) is shown:
  - as the idle caption (`CLAIM CODE K7M2-QX9P`, re-shown every 5 s while idle, since other
    screens borrow the caption);
  - in Muse's settings as the server status;
  - over BLE.

  It is not written to the serial log, as the wire doc asks. The serial log only says that code
  number N is ready. The `claim_secret` stays in RAM (`vc_claim_t`) and is wiped when the claim
  ends.
- **Poll.** `POST <host>/claim/poll` with `X-Claim-Secret` every 3 s.
  - `202`: keep polling.
  - `404 claim_not_found` (expired, replaced, wrong secret): start over, no sooner than 5.5 s
    after the last start, because the backend refuses a restart within 5 s.
  - `429`: wait for `Retry-After` (at most 900 s).
  - `503`: wait for `Retry-After`, else back off.
  - No response or a 5xx: back off 5 s doubling to 60 s.
  - `401`, `400`, or a `404` on start (wrong URL, or a backend from before task 08): look again
    in 60 s, with the reason as the caption.
  - Polling pauses while Muse rests (asleep on battery). A code nobody can see is no use, and the
    claim simply restarts on wake.
- **Claimed.** On `200` the credential is written to `muse:node_cred` at once. The write runs on
  a short-lived helper task with an internal-RAM stack, since the hatch task's stack is in PSRAM
  and must not touch flash. The caption shows `CLAIMED - <room>`. From then on every `/turn` and
  every `/audio` GET carries `X-Node-Credential`. If the NVS write fails, the credential is used
  from RAM until the next reboot, and then the node claims again.
  - Store and forget are serialised by a mutex in `vesper_cred.c`, and a forget always wins
    over a claim request it overlaps. The forget bumps a generation before it waits for the mutex, and
    the store samples it before it takes the mutex. `claim_step` publishes "claimed" only if
    the generation is unchanged and the credential is still in RAM. It checks again after
    publishing, and on its next pass it reconciles "claimed but no credential" into a normal
    re-claim. `vesper_cred.c` lists the interleavings. The NVS writer task never takes the mutex,
    so this can't deadlock.
- **Turns before the claim.** `muse_hatch_ready()` is false until the node is claimed. On the
  AIPI a press still records the note (the stock held-notes path), and it is sent once the node
  is claimed.
- **No refresh.** The deliverable "refresh-on-401 imitated from `app.c:860-935` if the
  credential model has refresh" resolves to **no refresh**. Task 08's model has no refresh token
  and the credential doesn't expire. The equivalent of stock's refresh-on-401 is
  **re-claim on `403 node_unauthorized`**. A `401` means the shared bearer is wrong, which no
  refresh could fix, so it stays a `TOKEN REFUSED` caption.
- **BLE.** The node's BLE host (`vesper_ble.c`, started from Link's keeper task on first need,
  as the stock server was) carries two services:
  - Muse's phone-setup service, `muse_ble.c` unchanged: `wifi.ssid`/`wifi.pass`/`wifi.connect`,
    `hatch.*`, the STATUS JSON, passkey pairing with the code on screen;
  - the Vesper claim service `76657370-6572-4e6f-6465-000000000001`. Its characteristic
    `…0002` (READ/NOTIFY) holds `{"state":"pending","code":"K7M2-QX9P"}`,
    `{"state":"starting"}` or `{"state":"claimed","room":"kitchen"}`. It needs no pairing: the
    code is on the screen anyway and is useless without the claim secret. It never carries the
    secret or the credential.

  The node advertises as `MuseGadget-XXXXXX` while a claim is in progress or while Muse's BLE
  setting is on. No Meta pairing, Muse-app setup protocol or Meta endpoint is involved.
  Cosmetic gap: the kept-identical UI shows the BLE icon from `muse_ble_status()`, which counts
  only the setting, so during a claim with the setting off the icon stays dark while the node
  advertises.

### NVS encryption: decision (task 11)

**Decision: NVS stays plaintext on the dev boards. Nothing is burned and flash encryption stays
off. The credential's protection is per-node revocation, not storage secrecy.**

What was weighed:

- **What the SDK offers.** `CONFIG_HOMEHUB_NVS_ENCRYPTION` (off; `config_store.c:39-45` reads
  it) selects ESP-IDF's HMAC-based NVS encryption. On first boot it generates an HMAC key into an
  eFuse key block and read-protects it. The NVS keys are derived from it in hardware.
- **Irreversibility.** That burn is one-way: the key block and its purpose can never be changed
  again. The S3 has 6 key blocks, and secure boot and flash encryption want some of them later.
  A dev board that has been burned to try it is committed to that layout for good.
- **What it would buy.** Without flash encryption, NVS encryption protects the NVS partition
  only. The firmware stays readable and rewritable over USB. Without secure boot, anyone with the
  board can flash a small app that uses the same HMAC peripheral to decrypt NVS. So it protects
  against a raw flash dump, and against nobody who can run code. A real defence needs secure
  boot v2, flash encryption in release mode and disabled USB/JTAG download. Each of those is a
  one-way eFuse step that ends the bench workflow (`board.sh flash`, serial monitor). That
  belongs with production signing and OTA (task 13), not with a dev board.
- **Threat model.** The credential is worth little alone. It works only with the shared edge
  bearer, which sits in the same plaintext NVS (`muse:node_token`), as did every stock
  credential before (`muse:token`, Link's tokens). It only lets its holder speak as that one node
  in its room through `/turn` and `/audio`. It can be revoked alone
  (`vesper-node nodes revoke homelink-<mac>`) without re-keying the fleet. Stealing it needs
  physical access to the board. A lost or stolen node is handled by revoking it.
- **Revisit** when the fleet is built for real: enable secure boot v2, flash encryption (release)
  and `CONFIG_HOMEHUB_NVS_ENCRYPTION` together on fresh production boards.
- **Task 13 kept this decision.** OTA works on the dev boards as they are, without burning
  anything; see *Firmware updates* for what protects an update instead (https only, the
  manifest's SHA-256, the image signature, rollback). The production hardening above, plus a
  private release signing key, is still for the real fleet build.

### Credentials never logged: evidence (task 11 DoD)

- **Host test, every `make -C firmware test`.** `hatch/test/check_log_hygiene.py` parses every
  logging call in `hatch/*.c|h` and in every line the patch set adds (`ESP_LOG*`, `printf`,
  `fprintf`, `puts`, `ets_printf`, `muse_hatch_console`). It fails if one passes an identifier
  holding:
  - the credential;
  - the claim secret;
  - the bearer;
  - the `Authorization` buffer;
  - a header value;
  - the claim code.

  String literals and presence checks (`token[0] ? …`, `vesper_cred_present()`) are allowed. It
  checks itself first against known-bad and known-good calls.
- **Live test.** `make -C firmware live-claim` greps the harness output and the throwaway
  backend's log for the credential, the claim secret and the token, and for any `vnc_`/`vcs_`
  shape. It finds none.
- **By construction.**
  - The firmware's claim log lines carry only the code's ordinal, HTTP statuses, poll counts,
    the room and timings.
  - `>status` reports `"credential":true|false` and the claim state.
  - The BLE claim characteristic carries only the state, the code and the room.
  - The secret, the credential and the `Authorization` buffer are wiped after use:
    `turn_finish`, `claim_post`, `vc_wipe`, and the NVS writer's copy.

### Speech (task 10, F2)

Stock firmware never spoke gadget replies: `start_tts()` in `muse_chat_session.cpp:1503-1517` was
an empty slot that paced captions over silence (conflict C4). The Vesper backend fills it.

- **Where.** `on_message_done` hands `audio_url` (already resolved by `vp_resolve_audio_url`, so on
  the configured server) to `vesper_tts_slot_offer()`, which only records it: no I/O inside the
  SSE parser. `start_showing()` (the stock `start_tts()`) takes the finished messages in order. A
  message with a URL is fetched: `GET` with the turn's `Authorization` and `X-Node-Id`,
  `Accept: audio/mpeg`, no redirects, 5 s connect/headers timeout. It needs `200` and
  `audio/mpeg`, and at most 2 MiB. Anything else falls back to silent pacing. The URL is never
  logged, because its id is a capability.
- **Decode.** This is the stock `tts_data`/`decode` path, on the hatch task (32 KB PSRAM stack;
  minimp3 needs ~16 KB). The body is read in 20 ms polls into a 192 KiB PSRAM buffer. Reading is
  flow-controlled: it reads only while the buffer has room, so a long reply never drops bytes.
  minimp3 decodes while the 2 s reply buffer (`s_out`) has room for a frame's output. Until the
  download ends it holds back the last 1,445 bytes, because minimp3 drops a frame it can't see
  past. Stereo is downmixed.
- **Resampling.** The backend sends `mp3_22050_32`, and the speaker runs at 16 kHz. Stock code
  resampled with linear interpolation. Going down from 22.05 kHz, that folds 8-11 kHz back into
  the speech: a 10 kHz tone comes out at 6 kHz only 4.6 dB down (host test). `vesper_audio.c`
  uses a polyphase windowed-sinc filter instead:
  - 48 taps, Blackman window, 128 phases, cutoff at 0.45 of the lower rate;
  - exact rational time base, so it never drifts;
  - unity DC gain in every phase;
  - its 24-sample lag is flushed at the end of each message.

  Measured: 1 kHz passes at gain 1.0000 with 63 dB SNR, and the 10 kHz alias is at -78 dB. The
  table takes 25 ms to build on the S3 the first time, and is kept for later messages at the same
  rate. The bench MP3 self-test uses the same resampler.
- **Playback.** Decoded PCM goes into the same `s_out` stream buffer that `muse_voice.c`
  (unchanged) reads with `muse_hatch_turn_read()` and writes to the 16 kHz speaker at the
  configured volume. `voice.c` and `voice_player.c` are untouched. The AIPI doesn't compile
  `voice.c`.
- **Caption sync.** `muse_hatch_turn_caption(played)` pages the message by `played - pcm_start`
  over `pcm_frames`, which comes from the audio:
  - while decoding: the samples out so far, plus the bytes left at the measured bytes per sample.
    It doesn't use the frame header's bitrate: the backend's first frame claims 56 kbps in a file
    that averages 32;
  - once drained: exact.

  Captions are no longer paced over silence when there is speech. One log line per page turn
  (`caption: message M page P at X s of Y s (char C of N)`) is the sync evidence. In sync,
  `C/N` follows `X/Y`.
- **Failures never wedge the turn.** In every case the turn still ends:
  - no URL, a refused URL, connect/HTTP/type failure, or no decodable frame: the stock pacing
    (16 chars/s plus a 2 s hold);
  - download cut short or stalled (10 s without bytes): what arrived is spoken, then the rest of
    the caption is paced over silence at the speech's own rate;
  - the 180 s turn cap still applies.
- **Cancel.** A new press (`turn_cancel`) bumps the generation and drains `s_out`. Then
  `CMD_CANCEL`, a failure, a settings change or the time cap reaches `turn_finish()`, which calls
  `tts_stop()` and closes the GET. Every write to `s_out` checks the generation first, so a
  cancelled turn's audio can't reach the speaker after the drain. The one blocking step is the
  GET's connect plus headers (5 s at worst; ~25 ms on the LAN). A cancel during it is handled
  right after, and the mic backlog (8 s) covers a press made meanwhile.
- **Memory.** The MP3 buffer (192 KiB), one decoded frame and its 16 kHz output are allocated
  once at start, all in PSRAM. The decoder and resampler state (~20 KB) are PSRAM `.bss`. No turn
  path allocates, so no error path has anything to free. If the start-up allocation fails,
  replies are captions only.

### Host checks (no board)

- `make -C firmware test` runs the protocol core's unit tests under ASan and UBSan, with
  `-Wall -Wextra -Werror`. They cover:
  - header building;
  - SSE framing at every split size, with the 2 KB comment preamble;
  - CRLF/CR line endings;
  - concatenating multiple deltas;
  - error, done and cut streams;
  - `audio_url` resolution and refusal;
  - oversized lines and events;
  - UTF-8 truncation;
  - JSON escapes;
  - HTTP verdicts.

  The same target runs the speech helpers' tests (`hatch/test/test_vesper_audio.c`):
  - resampler length and drift: 3 s in gives 3 s out, split anywhere or fed per MP3 frame;
  - 1, 3 and 5 kHz pass on time and at level;
  - 9 and 10 kHz are filtered, compared against the stock linear interpolator;
  - DC exact, saturation, and 8/24/44.1 kHz input;
  - the coefficient table (unity gain, accumulator headroom);
  - MP3 buffer hold, flow control and drain, with a stand-in decoder that, like minimp3, needs
    the next header;
  - the caption-length estimate and the TTS Content-Type check.

  And the claim flow's tests (`hatch/test/test_vesper_claim.c`, task 11):
  - code, secret, credential and room validation, including header injection, the exact
    backend shapes, length limits and an over-long secret that the JSON decoder cuts and the
    check then refuses;
  - the claim and credential headers;
  - the happy path: start, code on the caption, polls every 3 s, `202`, `200`, the credential
    taken once, the secret wiped;
  - every start failure: no response with its 5-60 s backoff, `429` with and without
    `Retry-After`, `503`, `401`, `400`, `404` from a pre-task-08 backend, and malformed `200`s;
  - every poll failure: `404` restarts no sooner than the backend's 5 s restart interval,
    transient errors keep the code, and a malformed `200` restarts;
  - reclaim on `403 node_unauthorized`, and a config change mid-claim;
  - 20,000 fuzzed bodies, none of which claims.

  `make test` also runs the log-hygiene check (see *Credentials never logged* above).
- `make -C firmware live-claim` runs the claim ceremony on the host, through the firmware's own
  `vesper_claim.c` with libcurl, against a **throwaway** backend (`hatch/test/live_claim.sh`):
  - It starts `backend/.venv/bin/vesper-node serve` from this checkout on a free loopback port
    (from 18796, never 8796), with a temp registry (`VESPER_NODE_REGISTRY_FILE` in a 700 temp
    dir), dummy tokens and both env files pointed at nothing.
  - The node starts a claim and polls. The script approves the code with
    `vesper-node claim <code> --room test`, and the poll collects the credential into an NVS
    stand-in file (mode 600).
  - A second run, standing in for a reboot, checks the results: `/turn` with the stored
    credential gets past auth (`422 bad_audio` on a junk note), and so does `/audio`
    (`404 not_found`). Without the credential, or with a wrong one, the backend answers
    `403 node_unauthorized`, which the firmware turns into "claim again".
  - It greps everything for leaked secrets, then kills only the backend it started.
- `make -C firmware live-turn` runs one real turn against the running node backend
  (`http://[::1]:8796` by default, `VESPER_NODE_URL` to override). It uses the same
  `vesper_proto.c` and libcurl in place of `esp_http_client`:
  1. `say` and `afconvert` make a 16 kHz mono PCM16 note.
  2. The note is sent as the firmware's streaming WAV, chunked.
  3. The SSE reply is parsed in 37-byte pieces.
  4. The resolved MP3 URL is fetched with the bearer.

  The token is read from `~/.config/vesper-voice/node.env` at runtime and never printed. It
  sends no node credential. Against a task-08 backend, its node id `homelink-hosttest` has to be
  registered with `--allow-shared-token`, or it gets `403 node_unauthorized`. `live-claim` is
  the credential path.

### Simulator

The simulator (`esp32/simulator/CMakeLists.txt`) builds only the UI (`muse_ui`, `muse_state`,
`muse_text` and the default avatar). It compiles no transport, so the backend swap leaves it as it
was, and `ctest` stays green on the patched tree. It doesn't exercise the Vesper backend. The host
tests above do that.

### On-device check (task 09 DoD, human gate)

The physical board is not flashed by the agents. Steps for Kevin:

1. **Get the backend reachable from the board.** Task 12's Peggy handle is live (2026-10-08), so
   the host is `https://peggy.fly.dev/vesper-node` from any network. For a LAN-only test instead:
   the backend binds `::`, which is IPv6-only on the Studio, so either give the board an IPv6
   route or add a temporary IPv4 forward on the Studio, for example:

   ```sh
   socat TCP4-LISTEN:8797,fork,reuseaddr TCP6:[::1]:8796
   ```

   Then use `http://<studio-LAN-IPv4>:8797` as the host.
2. **Build, flash and provision:**

   ```sh
   git -C ~/builds/muse-charm/scratch/muse-gadget-sdk worktree add --detach ~/builds/muse-charm/scratch/sdk-vesper b1a3822
   firmware/apply-sdk.sh ~/builds/muse-charm/scratch/sdk-vesper
   cd ~/builds/muse-charm/scratch/sdk-vesper/esp32
   tools/muse/board.sh build aipi && tools/muse/board.sh flash aipi /dev/cu.usbmodem83201
   . ~/esp/esp-idf-v6.0.1/export.sh && idf.py -B build-muse-aipi -p /dev/cu.usbmodem83201 monitor
   ```

   In the monitor, type the `>wifi.*` and `>hatch.*` lines above, then `>hatch.test`. Expect
   `server check: HTTP 200`.
3. **Run a turn.** Hold the talk button, say "what is two plus two", and release. Expect:
   - the screen to show `"what is two plus two"`, then the reply caption;
   - the serial log to show `turn: HTTP 200`, `transcript: N chars`,
     `message 0 done (... with speech)` and `turn done`;
   - the backend log (`~/Library/Logs/vesper-node/`) to show the turn with
     `X-Node-Id homelink-<mac>`.

   To roll back to stock, flash a build from the untouched SDK clone.

### On-device check (task 10: spoken replies)

Done by the task-10 agent on 2026-10-08, with the coordinator's authorisation, on the AiPi at
`/dev/cu.usbmodem83201` (Wi-Fi already provisioned). Build from `sdk-impl-10`, flashed with
`board.sh flash aipi`. `hatch.host=http://192.168.5.16:8797` (the socat stopgap) and
`hatch.token` were set over serial, piped from `node.env` and never printed. `>hatch.test` gave
`server check: HTTP 200`. The question was "what is two plus two", played from the Studio's
speakers into the board's mic. The full logs are in `/tmp` (not committed). The serial log of
the first turn:

```
vesper_chat: voice note: 4.04s, 129324 bytes
vesper_chat: turn: HTTP 200
vesper_chat: transcript: 21 chars
vesper_chat: message 0 done (21 chars, with speech)
vesper_chat: tts: message 0: GET HTTP 200, Content-Length 5347, audio/mpeg, headers in 23 ms
vesper_chat: tts: fetched 5347 of 5347 bytes in 26 ms
vesper_chat: tts: reply audio 22050 Hz, 1 ch, 56 kbps -> 16000 Hz (resampler ready in 26 ms)
muse_voice: reply audio after 4.21s
vesper_chat: tts: message 0: 50 MP3 frames at 22050 Hz -> 20898 samples at 16000 Hz (1.31 s of speech); first audio 57 ms after the GET
vesper_chat: turn done: 1 message(s) in 8.1s
vesper_chat: caption: message 0 page 2 at 0.95 s of 1.31 s (char 15 of 21)
muse_voice: muse reply: 1.31s of audio, 5.52s total
```

- The speech ran 1.31 s, and 20,898 samples at 16 kHz is 1.306 s. That matches 50 MPEG-2
  layer III frames x 576 samples at 22,050 Hz, which is 1.306 s, so the resampling keeps the
  duration.
- `muse_voice` played 1.31 s of it to the speaker.
- The caption turned its page at 0.95 of 1.31 s (72%) at character 15 of 21 (71%).

A second turn (the reply "Four.": 2,944 bytes, 27 frames, 11,285 samples, 0.71 s played) was
just as clean. The PSRAM heartbeat was back at its idle level afterwards (5,223 KiB free), so
nothing leaks.

**Barge-in.** A press 0.2 s into a 1.31 s reply logged `reply interrupted`, then
`turn cancelled`. Nothing more of the old reply played. The 0.14 s press was dropped as too
short, so it never reached `/ask`.

**HUMAN GATE (Kevin): hearing it.** The serial evidence shows the PCM going to the speaker. Only
a person can confirm the sound. With the board flashed and provisioned as above (it still is):

1. Make sure the backend (`com.vesper.node`) and the socat forward on `:8797` are running.
2. Hold the talk button, say "what is two plus two", and release.
3. Within about 5-8 s you should hear Vesper's voice say the answer from the AiPi's speaker,
   with the caption paging along with it.
4. If it's silent but the log shows `muse reply: X s of audio` with X > 0, check the volume in
   Muse's settings (the speaker plays at `muse_settings_volume()`).

### On-device check (task 11: the claim ceremony, human gate)

The agents don't flash the board or open its serial port. Steps for Kevin (or Vesper):

1. **Run the task-08 backend.** The claim routes and the credential check are task 08's code.
   Restart `com.vesper.node` from the main checkout so it runs that code:

   ```sh
   cd ~/builds/muse-charm/muse-charm && git pull && make -C backend install
   launchctl kickstart -k gui/$(id -u)/com.vesper.node
   make -C backend check-config            # "node registry: ...", "admin claim route: off"
   curl -s http://[::1]:8796/healthz       # {"ok": true}
   ```

   The board reaches it at `hatch.host=https://peggy.fly.dev/vesper-node` (task 12, live), or
   over the LAN via the socat forward on `:8797` as in task 09.
2. **Build and flash** (the agent's tree `~/builds/muse-charm/scratch/sdk-impl-11` is already
   patched; or apply the patch set to a fresh `b1a3822` worktree):

   ```sh
   firmware/apply-sdk.sh ~/builds/muse-charm/scratch/sdk-impl-11
   cd ~/builds/muse-charm/scratch/sdk-impl-11/esp32
   tools/muse/board.sh build aipi && tools/muse/board.sh flash aipi /dev/cu.usbmodem83201
   . ~/esp/esp-idf-v6.0.1/export.sh && idf.py -B build-muse-aipi -p /dev/cu.usbmodem83201 monitor | tee /tmp/claim-serial.log
   ```

   The bench board keeps its Wi-Fi, `hatch.host` and `hatch.token`, and has no `node_cred`, so
   it boots unclaimed. For a truly fresh board, erase it first (`idf.py -B build-muse-aipi -p
   /dev/cu.usbmodem83201 erase-flash`) and provision it as in task 09.
3. **The claim code on the screen.** Expect in the monitor:
   - `vesper_cred: node credential: none yet (the node will claim)`;
   - `vesper_ble: BLE host started`, then `advertising as MuseGadget-XXXXXX (claim in progress)`;
   - `vesper_chat: claim: code 1 ready (HTTP 200 in N ms); shown on the screen and over BLE`.

   The screen shows `CLAIM CODE XXXX-XXXX`, as does Settings > server status. Optional: in a
   BLE scanner (e.g. nRF Connect), `MuseGadget-XXXXXX` has service `76657370-…-000000000001`.
   Reading its characteristic gives `{"state":"pending","code":"XXXX-XXXX"}`.
4. **Approve it** on the Studio, with the code from the screen and the board's room:

   ```sh
   cd ~/builds/muse-charm/muse-charm/backend && uv run --locked vesper-node claim XXXX-XXXX --room <room>
   ```

   Within about 3 s the monitor shows
   `claim: claimed, room <room>; credential saved in NVS`, and the screen
   `CLAIMED - <room>`. Then `uv run --locked vesper-node nodes list` shows
   `homelink-c86320 room=<room> access=credential` (the shared-token transition, if it was on,
   is cleared). `>status` shows `"vesper":{...,"credential":true,"claim":"claimed"}`.
5. **Reboot** (the reset button, or Ctrl-T Ctrl-R in the monitor). Expect
   `node credential: stored` and no `claim:` lines.
6. **A push-to-talk turn.** Hold the talk button, say "what is two plus two", release. Expect:
   - `turn: HTTP 200` and the spoken reply;
   - the backend log (`~/Library/Logs/vesper-node/`) to show
     `turn done: node=homelink-c86320 room=<room> auth=credential ok=True`.
7. **No secret on serial.** Run `grep -cE 'v(nc|cs)_' /tmp/claim-serial.log`. It should print
   `0`. The code itself only ever appears on the screen and over BLE, never in the log.
8. Optional, the refusal path: `vesper-node nodes revoke homelink-c86320`, then press. Expect:
   - `turn: HTTP 403 node_unauthorized`, then `the node credential was refused; claiming again`;
   - `NODE NOT CLAIMED`, then a new `CLAIM CODE` on the screen.

   Approve it as in step 4.

### Firmware updates (task 13, F4)

The SDK's `ota.c` is kept (spec §2) and now updates the node from the Vesper node backend. Its
only trigger used to be the Meta app's BLE pairing (deleted in task 09). The wire side is
`docs/node-wire-protocol.md`, *Firmware updates*.

- **Version.** `firmware/hatch/VERSION` (`1.0.0`) is the image's `PROJECT_VER`
  (`esp32/CMakeLists.txt`, patch `0005`; it replaces the SDK's `version.txt`, `999.0.0`).
  `MAJOR.MINOR.PATCH` only. Bump it for every release.
- **Check** (`vesper_ota.c`, pure C and host-tested; the I/O in `muse_chat_vesper.c`, on the
  hatch task between turns). A claimed, online node with an `https://` server URL runs
  `GET <host>/firmware/manifest` (bearer, `X-Node-Id`, `X-Node-Credential`,
  `X-Node-Firmware`):
  - 10 s after boot, then every 6 h plus a fixed per-node jitter of up to 30 min;
  - at once on `>ota.check` (serial);
  - a failure backs off from 1 min, doubling, up to 6 h;
  - except on probation (a fresh image, `PENDING_VERIFY`, before its first answered check): a
    failed check is retried every 20 s, no doubling, so about 15 checks fit app.c's 300 s window
    and a few minutes of server or edge downtime right after the update (a backend restart, say)
    doesn't roll back, and blacklist, a good image. The backoff alone fits only 3.

  Why this trigger: a boot check makes "reboot it" the way to pull an update at once (a power
  cycle does it); the 6 h poll means a fleet converges within a few hours with no push channel
  and no new inbound port on the node (it has none, and Peggy only proxies node-initiated
  requests); the serial command covers the bench. A push from the backend would need the node
  to hold a connection open, which v1 doesn't.
- **Decision.** Install only a version strictly newer than the running one; never an equal or
  older one (no downgrade), never one this node rolled back before (read from the invalid slot
  at boot), never one larger than the update slot. A running version that isn't
  `MAJOR.MINOR.PATCH` (a dev build) is never updated over the air.
- **https only.** The image signing key is still the SDK's shared development key
  (`dev_signing_key.pem`, public, `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`). So the
  transport must be trusted: a node whose `hatch.host` is `http://` (the LAN socat stopgap)
  skips update checks (`>status` `"update":"needs_https"`) and keeps working for turns. Use
  `https://peggy.fly.dev/vesper-node` for nodes that should be updated.
- **Install** (`main/ota.c`, `ota_start_request`, on its own internal-RAM task, 16 KB stack;
  the node keeps answering turns while it downloads):
  1. nothing while the running image is still `PENDING_VERIFY` (its slot is the only known-good
     one);
  2. `GET <host>/firmware/<sha256>.bin` with the same headers, no redirects (so they can't
     leave the server), into the other app slot;
  3. the image's descriptor must carry exactly the manifest's version, and ota.c's own stock
     "newer than running" gate still applies;
  4. exactly `size` bytes, and the slot read back must hash to the manifest's SHA-256, all
     **before** `esp_https_ota_finish()`;
  5. `esp_https_ota_finish()` verifies the image (its own SHA-256 and RSA signature) and
     switches the boot partition;
  6. the restart waits for the node to be idle (a turn in progress finishes; at most 2 min);
  7. if the node lost its credential meanwhile (`>claim.forget`, a setup reset, a `403`), the
     update is dropped instead: the boot partition goes back to the running image (marked valid),
     so the restart ota.c still does comes back on the old firmware, which claims again and is
     offered the update again. Booting the new image unclaimed would fail its channel check, roll
     back and blacklist a good version.
- **Coming back.** The new image boots `PENDING_VERIFY`. `app.c`'s `ota_verify_task` keeps the
  stock 300 s window but, for a fresh image, now needs Wi-Fi **and** an answered update check
  (`200` or `204` with the node's credential). That only happens if the node still has its
  credential and the backend accepts it, so "comes back claimed" is enforced, not just hoped
  for. Otherwise the bootloader rolls back, and the old image never installs that version
  again. NVS (Wi-Fi, server URL, bearer, `node_cred`) is a separate partition, so an update
  never touches the node's identity.
- **Logs.** Versions, sizes, HTTP statuses, verdicts and timings only. The bearer and credential
  live in `s_turn.auth`/stack buffers that are wiped after each request, and in ota.c's copies,
  which are wiped when the OTA ends. `check_log_hygiene.py` covers the new code and patch.
- **Operator** (on the Studio):

  ```sh
  echo 1.0.1 > firmware/hatch/VERSION && git commit -am "firmware 1.0.1"   # the release
  firmware/apply-sdk.sh <SDK> && (cd <SDK>/esp32 && tools/muse/board.sh build aipi)
  cd backend && uv run --locked vesper-node firmware publish <SDK>/esp32/build-muse-aipi/muse-gadget.bin
  uv run --locked vesper-node firmware status        # what is published
  uv run --locked vesper-node firmware withdraw      # stop a rollout
  ```

  `publish` checks the image (ESP32-S3, `muse-gadget`, `MAJOR.MINOR.PATCH`, at most 4 MiB, newer
  than the published version) and stores it in `~/.config/vesper-voice/firmware/` (mode 700). The
  backend's log shows each node's check (`firmware check: node=... running=... published=...`)
  and download, which is how to watch a rollout.
- **Host checks.** `make -C firmware test` runs `test_vesper_ota` (versions, manifest parsing incl.
  truncated and fuzzed bodies, header injection, the verdicts, https-only, the schedule and its
  jitter). `make -C firmware live-ota [OTA_IMAGE=<build>/muse-gadget.bin]` runs the node's update
  code against a throwaway backend (temp registry and store, dummy tokens) and checks the
  download's size and SHA-256, up-to-date, no downgrade, a refused credential and a withdrawn
  release. `make -C backend verify` covers the routes (auth, wrong credential, revoked node, path
  traversal, tampered or insecure store) and the CLI.

### Fleet notes (task 13)

- **One firmware.** Every node runs the identical image. A node is made distinct only by its NVS
  identity: Wi-Fi, `hatch.host`, `hatch.token` (the shared edge bearer), and the `node_cred` its
  claim gave it. The node id is not configured at all: it is `homelink-<mac>`
  (`identity_node_id()`), from the chip.
- **N nodes = N registry rows, N claim ceremonies, one backend.** Each node is flashed once over
  USB with the current release, provisioned (Wi-Fi, server URL, bearer; serial or BLE), and
  claimed by reading its on-screen code (`vesper-node claim XXXX-XXXX --room <room>`). That
  gives it its row in `nodes.json` (room, credential hash). One `com.vesper.node` serves them
  all; the room comes from the registry, not the firmware.
- **Rolling an update to the fleet:** bump `hatch/VERSION`, build, `vesper-node firmware
  publish`. Every claimed node with an https server URL installs it within about 6.5 h (or at
  once after a reboot or `>ota.check`), and keeps it only if it gets back to the backend with
  its credential. Watch `firmware check`/`firmware image` lines in the backend log; a node still
  reporting the old version after a check has refused it (see its serial log) or rolled it back.
  To stop a bad rollout: `vesper-node firmware withdraw` (nodes that already took it keep it;
  publish a newer fixed version to move them on, since nodes never downgrade).
- **A revoked node gets no updates** (`403` on the manifest), like it gets no turns.
- **Not built here** (anti-deliverable): the multi-room expansion itself. The registry, rooms and
  updates are per node already; adding nodes is the procedure above.
- **Before a real fleet:** replace `dev_signing_key.pem` with a private release key kept out of
  git (each node's running image then needs one USB flash signed with the new key, since the
  running image's key verifies the next), and the production hardening in *NVS encryption:
  decision*.
- **Follow-ups (review advisories, not done in task 13):**
  - **Generate the private release key before more nodes are USB-flashed.** Every node flashed
    with a dev-key image has to be USB-flashed again to move to the release key; doing it first
    keeps that to one board. Optionally, have `vesper-node firmware publish` check the image's
    signature block against the release public key, so a wrongly signed build is refused on the
    Studio rather than by every node.
  - **Cache or stream the image in `FirmwareStore`.** `GET /firmware/<sha256>.bin` re-reads and
    re-hashes up to 4 MiB per request. Fine for a few nodes; for a fleet, verify once per
    publish (or per file change) and stream the file.

### On-device check (task 13: an OTA served by our backend, human gate)

Done by the task-13 agent on 2026-10-08, with the coordinator's authorisation, on the AiPi at
`/dev/cu.usbmodem83201`: the board was USB-flashed with the task-13 build (`1.0.0`, from a fresh
`b1a3822` worktree via `apply-sdk.sh`). Serial log:

```
link.app:   Version:  1.0.0
vesper_cred: node credential: none yet (the node will claim)
vesper_chat: firmware 1.0.0; updates from the node backend (first check 10 s after the node is claimed and online)
link.app: OTA image validated (Wi-Fi up)
vesper_chat: claim: start: refused (HTTP 404); next try in 60 s
@status {..."vesper":{"node_id":"homelink-c86320","credential":false,"claim":"starting","firmware":"1.0.0","update":"not_checked"},...}
@ota.check ok      (no check: the node isn't claimed)
```

The board is **unclaimed** (the live registry has no nodes), and the live `com.vesper.node` still
runs code from before task 08 (`/claim/start` is a 404), so the OTA itself could not run on the
device: claiming needs a person to read the code off the screen (task 11's human gate). Steps
for Kevin, once this branch is merged:

1. **Backend with the task 08 + 13 code** (claim and firmware routes):

   ```sh
   cd ~/builds/muse-charm/muse-charm && git pull && make -C backend install
   launchctl kickstart -k gui/$(id -u)/com.vesper.node
   make -C backend check-config     # "node firmware: ... published: none"
   ```
2. **Point the board at Peggy** (updates need https): in a serial monitor
   (`idf.py -B build-muse-aipi -p /dev/cu.usbmodem83201 monitor`, ESP-IDF exported, in
   `~/builds/muse-charm/scratch/sdk-impl-13-fresh/esp32`) type
   `>hatch.host=https://peggy.fly.dev/vesper-node`, then `>hatch.test` (expect `HTTP 200`).
3. **Claim it** (task 11's gate): read `CLAIM CODE XXXX-XXXX` off the screen, then
   `cd backend && uv run --locked vesper-node claim XXXX-XXXX --room <room>`. Within ~10 s the
   log shows `claim: claimed, room <room>` and then
   `update check: up to date (HTTP 204 ...; running 1.0.0)`.
4. **Publish 1.0.1** (already built from the same tree with `VERSION` = `1.0.1`):

   ```sh
   uv run --locked vesper-node firmware publish ~/builds/muse-charm/scratch/ota-test/muse-gadget-1.0.1.bin
   ```
5. **Update:** type `>ota.check` (or reboot the board, or wait up to ~6.5 h). Expect, in order:
   - `update check: newer version published (HTTP 200 ...; running 1.0.0, published 1.0.1)`;
   - `update: installing 1.0.1 (2035712 bytes) over 1.0.0`;
   - `link.ota: image matches the manifest (2035712 bytes, SHA-256)`;
   - `update: installed and verified; restarting into it once the node is idle`, a restart;
   - `Version:  1.0.1`, `running a PENDING_VERIFY OTA image; awaiting health check`,
     `node credential: stored`, `update check: up to date (HTTP 200 ...; running 1.0.1, published 1.0.1)`,
     `OTA image validated (Wi-Fi up, update server answered)`.

   The backend log shows `firmware image: served node=homelink-c86320 version=1.0.1`.
6. **Claimed and working:** `>status` shows `"credential":true,"claim":"claimed","firmware":"1.0.1"`;
   `vesper-node nodes list` still shows the node with `access=credential`; a push-to-talk turn
   gets a spoken reply.
7. **No secret on serial:** `grep -cE 'v(nc|cs)_' <the monitor log>` prints `0`.

To go back to `http://192.168.5.16:8797` (the LAN stopgap) afterwards, set `>hatch.host=` again;
turns work over either, updates only over https.

## Decision: the avatar is a patch set tracked in THIS repo (not an SDK fork)

`firmware/avatar/muse_pixel.c` is the source of truth. `firmware/avatar/install.sh [SDK_DIR]`
copies it (idempotently) to `<SDK>/esp32/components/muse/avatar/muse_pixel.c`.

Why:

- The SDK already has an explicit hook for this: `components/muse/CMakeLists.txt` globs
  `components/muse/avatar/muse_pixel.c` and uses it in place of the default avatar. That directory
  is ignored by the SDK repo, so installing the file touches no tracked SDK file and the SDK stays
  a pristine, re-clonable upstream checkout.
- The avatar is independent of transport. Task 09 (F1) later chose the same approach for the
  transport (see above), and `apply-sdk.sh` calls `install.sh`.
- The art is Vesper-owned; keeping it here keeps it out of any Meta-derived tree and reviewable
  next to the specs.

Caveat: the simulator (`esp32/simulator/CMakeLists.txt`) hard-codes the default avatar path
(`../../avatar/muse_pixel.c`) and does not use the custom one. The simulator `ctest` therefore does
not exercise Vesper's renderer; the avatar is covered by `make_gifs.py` (host render of every
mode) and by the ASan/UBSan bench run below. Making the simulator use it would mean editing a
tracked SDK file, which this task does not do.

## The avatar: Vesper, the evening-star owl

Original 64x64 palette-indexed art (31 palette entries, Bayer-dithered shading, 1 px outline).
A plump indigo owl with a pale moon face disc, ear tufts, big dark glinting eyes, small gold beak
and feet, and the evening star (a four-point sparkle) on its forehead. The star, aura, sparkles
and UI accent take the mode colour: gold at idle (a deliberate deviation from the SDK's violet
default), cyan listening, magenta thinking, mint speaking, red error, white/blue boot, dusk
violet off.

| Mode | Behaviour |
|---|---|
| BOOT | pops up from a squash, eyes open at 0.9 s, star ignites at 1.1 s, sparkles appear one by one |
| IDLE | slow bob, wings sway, random ear flicks, blinks (with double blinks), wandering gaze, pulsing star |
| LISTENING | ears perked, wide eyes, small open beak, raised brows, wings raised like cupped ears, expanding rings and sound arcs, star pulses with `level` |
| THINKING | eyes glance up and side to side, "hmm" beak, one wing folded to the chin, lean, one ear up, stepping thought dots, fast star flicker |
| SPEAKING | beak opens with `level` (plus flutter), body bobs with the voice, wings gesture, feet shuffle, rings and arcs, star flares with `level` |
| ERROR | X eyes, flat beak, 0.6 s shake, drooping ears, "!" mark, red scheme, star flickers (`happy` ignored) |
| OFF | right wing waves goodbye, eyes close, ears droop, glow fades over 1.3 s |
| happy overlay | hop, wings up and wiggling, `^^` eyes, open grin, ears up, big star, two hearts rising |

Previews: `firmware/avatar/gifs/*.gif` (192 px, one per mode plus `happy`), generated with
`python3 tools/muse/make_gifs.py` in the SDK and shrunk by `firmware/avatar/shrink_gifs.py`.

## Render time vs the 10 ms S3 budget

`firmware/avatar/bench.c` times `muse_pixel_render()` plus a full 128x128 `muse_pixel_scale()` per
frame, for every mode with and without the happy overlay (1950 frames each, `cc -O2`):

    cc -O2 -I <SDK>/esp32/components/muse firmware/avatar/bench.c firmware/avatar/muse_pixel.c -lm -o /tmp/vesper_bench && /tmp/vesper_bench

Host (Apple M4 Max, arm64, -O2): render mean 11-15 us (max seen 53 us), scale128 mean ~3 us;
worst single frame render+scale 48 us. The same code builds clean with
`-Wall -Wextra -Werror` and runs clean under AddressSanitizer + UBSan.

S3 estimate (240 MHz Xtensa LX7, 1 single-precision FPU, no sqrt/sin hardware): the M4 is roughly
150-250x faster per core on this kind of scalar integer code (about 4.5 GHz at ~6 IPC vs 240 MHz at
~0.7-1 IPC). That puts render at about 2-4 ms and a full scale at about 0.5-1 ms: roughly
3-5 ms per frame, comfortably under 10 ms even with a 2x safety margin for internal-RAM cache
misses. Per-pixel work is Q12 integer inside the body, wing, face and aura bounding boxes; floats
are used per frame or per part only: about 70 sqrtf (one per body row), a few dozen
sinf/cosf/expf, and the tiny eye/blush/foot ellipses (a few tens of pixels). On the S3's FPU these
are a few thousand cycles in total (under 0.3 ms), so there is no float/libm hot spot. No
allocation; static RAM is about 8 KiB of planes plus palettes and a 512-byte map. This is a host
proxy, not a measurement; confirm on hardware in task 05 if desired.
