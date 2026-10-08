/*
 * The Vesper backend of the muse_hatch_* seam (muse_chat.h): push-to-talk
 * turns go to the Vesper node backend over docs/node-wire-protocol.md v1.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources, except where a
 * function says it was adapted from one (Apache-2.0).
 *
 * Configuration (NVS namespace "muse", see muse_settings.c):
 *   host        server base URL, e.g. https://peggy.fly.dev/vesper-node
 *               (the turn goes to <host>/turn); serial: >hatch.host=<url>
 *   node_token  the shared node bearer; serial: >hatch.token=<token>
 *   node_cred   the per-node credential from the claim flow (task 11,
 *               vesper_cred.h); never typed in, never logged
 * host and node_token fall back to CONFIG_VESPER_NODE_URL / CONFIG_VESPER_NODE_TOKEN, which
 * are empty in the repo. The device id is identity_node_id() (homelink-<mac>).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vesper_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Set once at boot (muse_glue.c), before the first turn: the X-Node-Id. */
void muse_hatch_set_node_id(const char *node_id);

/*
 * Speech (task 10, F2): a reply message whose message_done carries an
 * audio_url is fetched (GET, same bearer, same server only), decoded and
 * resampled to 16 kHz on the hatch task, and handed to the voice task through
 * muse_hatch_turn_read(), its caption timed by the speech. With no audio_url,
 * or if the fetch or decode fails, the caption is paced over silence as in
 * stock firmware. See muse_chat_vesper.c and vesper_audio.h.
 */

/*
 * Claim flow (task 11): see muse_chat_vesper.c and vesper_claim.h.
 *
 * vesper_node_forget_credential() forgets the stored credential and claims
 * again (serial >claim.forget). The setup reset uses the _now form, which
 * erases it synchronously (it restarts right after); call it from a task with
 * an internal-RAM stack.
 */
void vesper_node_forget_credential(void);
bool vesper_node_forget_credential_now(void);
/* {"node_id":...,"credential":true|false,"claim":"claimed|starting|pending","firmware":"1.0.0","update":"..."}:
 * presence only, for >status. */
int vesper_node_status_json(char *out, size_t cap);

/*
 * Firmware updates (task 13, F4): see vesper_ota.h. A claimed node asks its
 * server for the published firmware VO_FIRST_CHECK_MS after boot, then every
 * 6 hours (plus a per-node jitter), between turns, and installs a strictly
 * newer version through the installer app.c registers (the SDK's ota.c,
 * ota_start_request).
 *
 * The installer gets everything it needs in *req (copy it: the header values
 * are wiped when it returns) and calls done(applied, detail) when the
 * install ends. applied: the device restarts as soon as done returns (done
 * waits for the node to be idle first). detail is a fixed, secret-free reason.
 */
typedef struct {
    const char *url;               /* <host>/firmware/<sha256>.bin */
    const vp_header_t *headers;    /* bearer, node id, node credential, running version */
    int nheaders;
    const char *version;           /* the image must carry exactly this version */
    const uint8_t *sha256;         /* 32 bytes: what the image must hash to */
    uint32_t size;                 /* its exact size */
} vesper_update_t;
typedef void (*vesper_update_done_t)(bool applied, const char *detail);
typedef void (*vesper_updater_t)(const vesper_update_t *req, vesper_update_done_t done);

void vesper_node_set_updater(vesper_updater_t updater);
/* Check for an update now (serial >ota.check), once the node is claimed and on Wi-Fi. */
void vesper_node_check_update(void);
/* The update server has answered a check since boot (a manifest, or nothing published).
 * app.c keeps a freshly installed image only once this is true (else it rolls back). */
bool vesper_node_update_channel_ok(void);

#ifdef __cplusplus
}
#endif
