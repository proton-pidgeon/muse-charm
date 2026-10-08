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

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set once at boot (muse_glue.c), before the first turn: the X-Node-Id. */
void muse_hatch_set_node_id(const char *node_id);

/*
 * ---- TTS slot (task 10, F2) ----
 *
 * Called on the hatch task when a reply message is complete and the server
 * made speech for it: abs_url is message_done.audio_url resolved against the
 * server URL (vp_resolve_audio_url guarantees it is on the configured server),
 * to be fetched with the same bearer. msg is the message's index in the turn.
 *
 * Return false to leave the message to the stock behaviour (its caption is
 * paced over silence). Return true only once the slot has taken over the
 * message's speech: it then GETs the MP3 and feeds the existing
 * decode / muse_hatch_turn_read path, and must finish the message.
 *
 * Task 09 ships only this weak default, which declines (no playback yet).
 */
bool vesper_tts_slot_offer(const char *abs_url, int msg);

#ifdef __cplusplus
}
#endif
