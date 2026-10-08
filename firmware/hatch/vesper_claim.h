/*
 * The node side of the claim flow (docs/node-wire-protocol.md, "Claim flow"):
 * the pure-C core the firmware (muse_chat_vesper.c) and the host tests share.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources.
 *
 * A node with no credential (fresh flash, setup reset, or a 403
 * node_unauthorized on /turn) runs:
 *
 *   POST <base>/claim/start   bearer + X-Node-Id
 *        200 {claim_code, claim_secret}   the code goes on the screen and BLE,
 *                                         the secret stays in RAM only
 *   POST <base>/claim/poll    bearer + X-Node-Id + X-Claim-Secret, every 3 s
 *        202 pending                      poll again
 *        200 {credential, room}           exactly once: store it in NVS
 *        404 claim_not_found              start over (expired, replaced, ...)
 *
 * and then sends X-Node-Credential on every /turn and /audio request. There is
 * no refresh token in this credential model: a credential that stops working
 * (403 node_unauthorized) sends the node back to the claim flow.
 *
 * This file does no I/O and keeps no clock: the caller passes the time in
 * milliseconds (any monotonic origin) and the HTTP results. Like
 * vesper_proto.c it has no ESP-IDF dependencies and never allocates. Nothing
 * here logs; the caller must never log the secret or the credential (the
 * structs below hold them, so never print a vc_claim_t).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vesper_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Sizes and wire constants ---- */

#define VC_CODE_LEN 9            /* "K7M2-QX9P": 8 characters shown as XXXX-XXXX */
#define VC_SECRET_MAX 100        /* "vcs_" + url-safe base64; the backend's is 47 */
#define VC_CRED_MAX 100          /* "vnc_" + url-safe base64; the backend's is 47 */
#define VC_ROOM_MAX 40           /* backend ROOM_RE: 1-40 characters */
#define VC_BODY_MAX 512          /* a claim response body; longer ones are cut and fail to parse */

#define VC_CODE_ALPHABET "23456789ABCDEFGHJKMNPQRSTVWXYZ"
#define VC_SECRET_PREFIX "vcs_"
#define VC_CRED_PREFIX "vnc_"
#define VC_CRED_HEADER "X-Node-Credential"
#define VC_SECRET_HEADER "X-Claim-Secret"

/* ---- Timing (milliseconds) ---- */

#define VC_POLL_MS 3000          /* the backend's poll_interval */
#define VC_RESTART_MS 5500       /* the backend refuses a restart within 5 s of the last start */
#define VC_BACKOFF_MIN_MS 5000   /* unreachable / server error: retry after this, doubling */
#define VC_BACKOFF_MAX_MS 60000
#define VC_REFUSED_MS 60000      /* 401/400/404 on start: the config is wrong; look again later */
#define VC_RETRY_AFTER_MAX_S 900 /* cap on a server's Retry-After */

/* ---- Validation ---- */

/* "XXXX-XXXX" from VC_CODE_ALPHABET, exactly. */
bool vc_valid_code(const char *code);
/* "vcs_" + 16..(VC_SECRET_MAX - 4) characters of [A-Za-z0-9_-]. Header-safe. */
bool vc_valid_secret(const char *secret);
/* "vnc_" + 16..(VC_CRED_MAX - 4) characters of [A-Za-z0-9_-]. Header-safe. */
bool vc_valid_credential(const char *credential);
/* The backend's ROOM_RE: letters, digits, space, '-', '_', 1-40, alnum at both ends. */
bool vc_valid_room(const char *room);

/* ---- Request headers ---- */

#define VC_CLAIM_HEADERS 5

/*
 * The headers of POST <base>/claim/start (secret NULL: 4 headers) or
 * POST <base>/claim/poll (secret set: 5 headers), in order: Authorization,
 * X-Node-Id, X-Vesper-Node-Protocol, Accept, [X-Claim-Secret]. Authorization
 * is written into auth_buf. Returns the number of headers, or 0 (and nothing
 * usable) if the token, node id or secret is invalid.
 */
int vc_claim_headers(const char *token, const char *node_id, const char *secret, char auth_buf[VP_AUTH_MAX],
                     vp_header_t out[VC_CLAIM_HEADERS]);

/* {"X-Node-Credential", credential} for /turn and /audio; false if it isn't a valid credential. */
bool vc_credential_header(const char *credential, vp_header_t *out);

/* A /turn (or /audio) reply that means "this node's credential doesn't work": claim again. */
bool vc_needs_claim(int status, const char *error_code);

/* Retry-After as delta-seconds, clamped to 1..VC_RETRY_AFTER_MAX_S; dflt if absent or not a number. */
int vc_retry_after_s(const char *retry_after, int dflt);

/* ---- The claim state machine ---- */

typedef enum {
    VC_CLAIMED,      /* a credential is held: nothing to do */
    VC_START,        /* POST /claim/start when due */
    VC_POLL,         /* a code is on the screen: POST /claim/poll when due */
} vc_state_t;

/* Why the last request didn't move the claim on (for the screen). */
typedef enum {
    VC_NOTE_NONE,
    VC_NOTE_UNREACHABLE,     /* no response */
    VC_NOTE_TOKEN_REFUSED,   /* 401: the shared node token is wrong */
    VC_NOTE_REFUSED,         /* 400: bad node id or request */
    VC_NOTE_NO_CLAIM_ROUTE,  /* 404/405 on start: wrong URL, or a backend from before task 08 */
    VC_NOTE_BUSY,            /* 429/503: the server asked to wait */
    VC_NOTE_SERVER_ERROR,    /* anything else, or a reply that didn't parse */
    VC_NOTE_EXPIRED,         /* the code expired or was replaced: getting a new one */
} vc_note_t;

typedef enum {
    VC_ACT_NONE,
    VC_ACT_START,
    VC_ACT_POLL,
} vc_action_t;

typedef enum {
    VC_EV_NONE,          /* nothing new to show */
    VC_EV_CODE,          /* a new code to show (vc_claim_t.code) */
    VC_EV_PENDING,       /* still waiting for approval */
    VC_EV_CLAIMED,       /* vc_claim_t.credential and .room are set: store the credential */
    VC_EV_RESTART,       /* the code is gone (expired/replaced/refused): a new one follows */
    VC_EV_PROBLEM,       /* see vc_claim_t.note */
} vc_event_t;

typedef struct {
    vc_state_t state;
    vc_note_t note;
    int64_t due_ms;          /* when the next request is due */
    int64_t last_start_ms;
    bool started;            /* last_start_ms is set */
    int32_t backoff_ms;
    uint32_t polls;          /* polls of the current code */
    uint32_t codes;          /* codes shown since boot */
    char code[VC_CODE_LEN + 1];
    char secret[VC_SECRET_MAX + 1];       /* RAM only, wiped when the claim ends */
    char credential[VC_CRED_MAX + 1];     /* set on VC_EV_CLAIMED, until vc_take_credential */
    char room[VC_ROOM_MAX + 1];
} vc_claim_t;

/* have_credential: a credential was found in NVS (state CLAIMED), else claim now. */
void vc_init(vc_claim_t *c, bool have_credential, int64_t now_ms);

/* What is due now (VC_ACT_NONE when claimed or not yet due). */
vc_action_t vc_due(const vc_claim_t *c, int64_t now_ms);
/* Milliseconds until the next request (0 if due now), or -1 when claimed. */
int64_t vc_wait_ms(const vc_claim_t *c, int64_t now_ms);

/*
 * The result of POST /claim/start: status (0: no response), the body (JSON,
 * may be NULL), its length, the Retry-After header (may be NULL).
 */
vc_event_t vc_start_result(vc_claim_t *c, int status, const char *body, size_t len, const char *retry_after,
                           int64_t now_ms);
/* The result of POST /claim/poll, the same way. */
vc_event_t vc_poll_result(vc_claim_t *c, int status, const char *body, size_t len, const char *retry_after,
                          int64_t now_ms);

/*
 * The stored credential was refused (vc_needs_claim) or forgotten: claim again,
 * now (or as soon as the backend's restart interval allows).
 */
void vc_reclaim(vc_claim_t *c, int64_t now_ms);
/* The server URL or token changed: a claim in progress starts over now. A claimed node stays claimed. */
void vc_config_changed(vc_claim_t *c, int64_t now_ms);
/* Copies the delivered credential into out (VC_CRED_MAX + 1) and wipes it here. False if there is none. */
bool vc_take_credential(vc_claim_t *c, char out[VC_CRED_MAX + 1]);
/* Wipes every secret in the struct (secret, credential). */
void vc_wipe(vc_claim_t *c);

/*
 * Widest caption the AIPI Lite's 128x128 screen shows whole. muse_ui.c's compact layout draws the
 * caption in one line of unscii_8 (8 px glyphs) across the 128 px width, which is 16 columns; any
 * longer text ends in "..." (task 15: "CLAIM CODE K7M2-QX9P" showed as "CLAIM CODE K7...").
 * Every vc_caption() text stays within it, so the whole XXXX-XXXX code is always on screen.
 */
#define VC_CAPTION_COLS 16
/* For the screen: "CODE K7M2-QX9P", "GETTING A CODE", "TOKEN REFUSED", ...; "" when claimed. All <= VC_CAPTION_COLS. */
void vc_caption(const vc_claim_t *c, char *out, size_t cap);
/* For status JSON: "claimed", "starting" or "pending". Never the code or a secret. */
const char *vc_state_name(const vc_claim_t *c);

#ifdef __cplusplus
}
#endif
