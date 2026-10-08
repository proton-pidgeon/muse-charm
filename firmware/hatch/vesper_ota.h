/*
 * Firmware updates from the Vesper node backend (task 13, F4): the pure-C
 * core the firmware (muse_chat_vesper.c) and the host tests share.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources.
 *
 * A claimed node asks its server what firmware is published:
 *
 *   GET <base>/firmware/manifest   bearer + X-Node-Id + X-Node-Credential
 *                                  (+ X-Node-Firmware: the running version)
 *       200 {"version":"1.0.1","sha256":"<64 hex>","size":2035712,...}
 *       204                        nothing published
 *
 * and, if the published version is newer than the one it runs, hands
 *
 *   GET <base>/firmware/<sha256>.bin   (same headers)
 *
 * to the SDK's kept ota.c (ota_start_request), which streams it into the
 * other app slot, checks that the image's own descriptor carries exactly the
 * manifest's version, that exactly `size` bytes arrived and that what is in
 * flash hashes to `sha256`, and only then lets esp_https_ota_finish() verify
 * the image (its SHA-256 and RSA signature) and switch the boot partition.
 * The new image boots PENDING_VERIFY and is kept only once it has reached
 * this server again (app.c, ota_verify_task); otherwise the bootloader rolls
 * back to the old one.
 *
 * Versions are plain MAJOR.MINOR.PATCH (firmware/hatch/VERSION, the build's
 * PROJECT_VER). A node installs only a strictly newer version: never an equal
 * or older one, and never one that was already rolled back on it.
 *
 * Like vesper_proto.c this does no I/O, keeps no clock (the caller passes
 * milliseconds) and never allocates.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vesper_claim.h"
#include "vesper_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VO_VERSION_MAX 31        /* esp_app_desc_t.version is char[32] */
#define VO_SHA_LEN 32
#define VO_BODY_MAX 512          /* a manifest body; longer ones are cut and fail to parse */
#define VO_FIRMWARE_HEADER "X-Node-Firmware"

/* ---- Timing (milliseconds) ---- */

#define VO_FIRST_CHECK_MS 10000              /* after boot, once the node is claimed and on Wi-Fi */
#define VO_PERIOD_MS (6LL * 3600 * 1000)     /* then every 6 hours */
#define VO_JITTER_MS (30LL * 60 * 1000)      /* + up to 30 min, by node id, to spread a fleet */
#define VO_RETRY_MIN_MS (60LL * 1000)        /* a failed check or install: retry after this, doubling */

/* ---- Versions ---- */

typedef struct {
    uint16_t major, minor, patch;
} vo_version_t;

/* "MAJOR.MINOR.PATCH", each 0..65535 in decimal without leading zeros. Nothing else. */
bool vo_version_parse(const char *s, vo_version_t *out);
/* <0, 0, >0 like strcmp. */
int vo_version_cmp(const vo_version_t *a, const vo_version_t *b);

/* ---- The manifest ---- */

typedef struct {
    char version[VO_VERSION_MAX + 1];
    vo_version_t v;
    uint8_t sha256[VO_SHA_LEN];
    char sha256_hex[2 * VO_SHA_LEN + 1];
    uint32_t size;
} vo_manifest_t;

/* 64 lowercase hex digits -> 32 bytes. */
bool vo_sha_parse(const char *hex, uint8_t out[VO_SHA_LEN]);

/* Parses a 200 manifest body. False (and *out zeroed) unless the version, sha256 and size are all well formed. */
bool vo_manifest_parse(const char *body, size_t len, vo_manifest_t *out);

/* <base>/firmware/<sha256>.bin: always on the configured server, never a URL from the body. */
bool vo_image_url(const vp_url_t *base, const vo_manifest_t *m, char *out, size_t cap);
/* <base>/firmware/manifest */
bool vo_manifest_url(const vp_url_t *base, char *out, size_t cap);

/* ---- Request headers ---- */

#define VO_HEADERS 4

/*
 * The headers of both GETs, in order: Authorization (written into auth_buf,
 * VP_AUTH_MAX), X-Node-Id, X-Node-Credential, X-Node-Firmware. False (and
 * nothing usable) if the token, node id, credential or running version is
 * not well formed (the same header-injection-safe checks as the turns).
 */
bool vo_headers(const char *token, const char *node_id, const char *credential, const char *running_version,
                char auth_buf[VP_AUTH_MAX], vp_header_t out[VO_HEADERS]);

/* ---- What to do with a manifest response ---- */

typedef enum {
    VO_UP_TO_DATE,       /* 204, or the published version is the running one */
    VO_INSTALL,          /* 200 with a newer, well-formed release that fits: install it */
    VO_SKIP_OLDER,       /* the published version is older than the running one: never a downgrade */
    VO_SKIP_REJECTED,    /* newer, but this exact version was rolled back on this node before */
    VO_SKIP_TOO_BIG,     /* larger than the app slot */
    VO_SKIP_UNVERSIONED, /* the running version isn't MAJOR.MINOR.PATCH (a dev build): update over USB */
    VO_BAD_MANIFEST,     /* 200 with a body that doesn't parse */
    VO_REFUSED,          /* 401/403: the bearer or the node credential was refused */
    VO_FAILED,           /* no response, a 5xx, or any other status */
} vo_verdict_t;

/*
 * status: the HTTP status (0: no response); body/len: the response body;
 * running: the running image's version; rejected: the version of an image
 * this node rolled back (esp_ota_get_last_invalid_partition), or "" / NULL;
 * slot_size: the size of the app slot an update goes into. *out is filled
 * for 200 responses that parse.
 */
vo_verdict_t vo_check_result(int status, const char *body, size_t len, const char *running, const char *rejected,
                             uint32_t slot_size, vo_manifest_t *out);

/* The update server answered as it should (a manifest, or nothing published): the
 * channel a future update comes through works. This is what a freshly installed
 * image must see before it is kept. */
bool vo_channel_ok(vo_verdict_t v);

/* A short, fixed name for logs and >status. */
const char *vo_verdict_name(vo_verdict_t v);

/* ---- When to check ---- */

typedef struct {
    int64_t next_ms;     /* the next check is due at this time */
    uint32_t failures;   /* consecutive failed checks or installs */
} vo_sched_t;

/* The first check: VO_FIRST_CHECK_MS from now. */
void vo_sched_init(vo_sched_t *s, int64_t now_ms);
bool vo_sched_due(const vo_sched_t *s, int64_t now_ms);
/* Milliseconds until the next check (0 if due). */
int64_t vo_sched_wait_ms(const vo_sched_t *s, int64_t now_ms);
/* A check (or install) ended: ok -> the next in VO_PERIOD_MS + the node's jitter; else back off
 * from VO_RETRY_MIN_MS, doubling, at most VO_PERIOD_MS. */
void vo_sched_done(vo_sched_t *s, bool ok, const char *node_id, int64_t now_ms);
/* Check now (serial >ota.check). */
void vo_sched_now(vo_sched_t *s, int64_t now_ms);
/* The node's fixed share of VO_JITTER_MS (a hash of its id), so a fleet doesn't check at once. */
int64_t vo_jitter_ms(const char *node_id);

#ifdef __cplusplus
}
#endif
