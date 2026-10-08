/*
 * Announcements from the Vesper node backend (task 18): the pure-C core the
 * firmware (muse_chat_vesper.c) and the host tests share.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources.
 *
 * While it is idle, a claimed node asks its server for due timers and
 * reminders:
 *
 *   GET <base>/announcements   bearer + X-Node-Id + X-Node-Credential
 *       200 {"announcements":[{"text":"Your ten minute timer is done.",
 *                              "audio_url":"audio/<id>.mp3"}, ...]}
 *       429 rate_limited (Retry-After)     403 node_unauthorized (claim again)
 *
 * and says each one: its text is the caption, its audio_url (same acceptance
 * rules as message_done.audio_url: vp_resolve_audio_url) goes through the
 * reply-MP3 path, a null or refused one leaves the caption paced over
 * silence. The server hands each announcement out once, so whatever a press
 * interrupts is dropped, never asked for again.
 *
 * Like vesper_proto.c this does no I/O, keeps no clock (the caller passes
 * milliseconds) and never allocates.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vesper_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VN_MAX 5                 /* the backend sends at most 5; more are ignored */
#define VN_TEXT_MAX 1024         /* a decoded text, with the NUL (= the turn's TEXT_MAX) */
#define VN_TEXT_BYTES_MAX 800    /* the backend caps a text at 200 chars: <= 800 UTF-8 bytes */
#define VN_BODY_MAX 8192         /* a 200 body; one this long or longer is refused whole */

/* ---- Timing (milliseconds) ---- */

#define VN_POLL_MS 15000         /* idle: every 15 s */
#define VN_BACKOFF_MAX_MS 60000  /* failures back off 15 s -> 30 s -> 60 s */
#define VN_RETRY_AFTER_MAX_MS (5LL * 60 * 1000)   /* a 429's Retry-After is believed up to this */

/* ---- The list ---- */

typedef struct {
    char text[VN_TEXT_MAX];      /* the caption: control characters made spaces */
    char audio[VP_URL_MAX];      /* the resolved MP3 URL, or "" (none, or refused) */
} vn_item_t;

typedef struct {
    vn_item_t items[VN_MAX];
    int n;
    unsigned dropped;            /* entries that weren't a usable announcement */
    unsigned audio_refused;      /* kept, but their audio_url failed vp_resolve_audio_url */
} vn_list_t;

/*
 * Parses a 200 body (len bytes, need not be NUL-terminated) against the server
 * base URL. False (and out->n == 0) if the body is too long (>= VN_BODY_MAX),
 * isn't one well-formed JSON object, or has no "announcements" array. Inside a
 * good array, only the first VN_MAX elements are looked at; an element that
 * isn't an object with a non-empty string "text" of at most VN_TEXT_BYTES_MAX
 * bytes is dropped (counted). "audio_url": a string is resolved with
 * vp_resolve_audio_url (a refused one, or one of another type, leaves the item
 * as a caption only); null or missing means a caption only.
 */
bool vn_parse(const char *body, size_t len, const vp_url_t *base, vn_list_t *out);

/* <base>/announcements. False if it doesn't fit. */
bool vn_url(const vp_url_t *base, char *out, size_t cap);

/* ---- What a poll's response means ---- */

typedef enum {
    VN_OK,           /* 200 that parses (possibly with nothing in it) */
    VN_BAD_BODY,     /* 200 that doesn't */
    VN_RATE_LIMITED, /* 429: wait for Retry-After */
    VN_REFUSED,      /* 401/403: the bearer or the credential was refused (403 node_unauthorized: claim again) */
    VN_FAILED,       /* no response, 5xx, anything else */
} vn_verdict_t;

vn_verdict_t vn_verdict(int status, bool parsed);
const char *vn_verdict_name(vn_verdict_t v);

/* ---- When to poll ---- */

typedef struct {
    int64_t next_ms;
    uint32_t failures;   /* consecutive polls that weren't VN_OK */
} vn_sched_t;

/* The first poll: VN_POLL_MS from now. */
void vn_sched_init(vn_sched_t *s, int64_t now_ms);
bool vn_sched_due(const vn_sched_t *s, int64_t now_ms);
/* Milliseconds until the next poll (0 if due). */
int64_t vn_sched_wait_ms(const vn_sched_t *s, int64_t now_ms);
/*
 * A poll ended. VN_OK: next in VN_POLL_MS. Otherwise back off: VN_POLL_MS
 * doubling per consecutive failure, at most VN_BACKOFF_MAX_MS; a 429 waits at
 * least its Retry-After (retry_after: the header, or NULL; at most
 * VN_RETRY_AFTER_MAX_MS).
 */
void vn_sched_done(vn_sched_t *s, vn_verdict_t v, const char *retry_after, int64_t now_ms);

#ifdef __cplusplus
}
#endif
