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
 *   node_token  the node bearer credential; serial: >hatch.token=<token>
 * Both fall back to CONFIG_VESPER_NODE_URL / CONFIG_VESPER_NODE_TOKEN, which
 * are empty in the repo. The device id is identity_node_id() (homelink-<mac>).
 */
#pragma once

#include <stddef.h>

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

#ifdef __cplusplus
}
#endif
