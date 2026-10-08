/*
 * Firmware updates from the Vesper node backend (task 13): see vesper_ota.h.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources.
 */
#include "vesper_ota.h"

#include <stdio.h>
#include <string.h>

/* ---- Versions ---- */

static bool parse_part(const char **p, uint16_t *out)
{
    const char *s = *p;
    uint32_t v = 0;
    int n = 0;
    while (*s >= '0' && *s <= '9') {
        if (n == 5) {
            return false;
        }
        v = v * 10 + (uint32_t)(*s - '0');
        s++;
        n++;
    }
    if (n == 0 || (n > 1 && **p == '0') || v > 0xFFFF) {
        return false;
    }
    *out = (uint16_t)v;
    *p = s;
    return true;
}

bool vo_version_parse(const char *s, vo_version_t *out)
{
    vo_version_t v;
    memset(out, 0, sizeof(*out));
    if (!s || strlen(s) > VO_VERSION_MAX) {
        return false;
    }
    if (!parse_part(&s, &v.major) || *s++ != '.' || !parse_part(&s, &v.minor) || *s++ != '.' ||
        !parse_part(&s, &v.patch) || *s != '\0') {
        return false;
    }
    *out = v;
    return true;
}

int vo_version_cmp(const vo_version_t *a, const vo_version_t *b)
{
    if (a->major != b->major) {
        return a->major < b->major ? -1 : 1;
    }
    if (a->minor != b->minor) {
        return a->minor < b->minor ? -1 : 1;
    }
    if (a->patch != b->patch) {
        return a->patch < b->patch ? -1 : 1;
    }
    return 0;
}

/* ---- The manifest ---- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

bool vo_sha_parse(const char *hex, uint8_t out[VO_SHA_LEN])
{
    memset(out, 0, VO_SHA_LEN);
    if (!hex || strlen(hex) != 2 * VO_SHA_LEN) {
        return false;
    }
    uint8_t tmp[VO_SHA_LEN];
    for (int i = 0; i < VO_SHA_LEN; i++) {
        int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        tmp[i] = (uint8_t)(hi << 4 | lo);
    }
    memcpy(out, tmp, VO_SHA_LEN);
    return true;
}

bool vo_manifest_parse(const char *body, size_t len, vo_manifest_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!body) {
        return false;
    }
    vo_manifest_t m;
    memset(&m, 0, sizeof(m));
    char sha[2 * VO_SHA_LEN + 2];
    char version[VO_VERSION_MAX + 2];
    /* The whole body must be one well-formed object (looking up a key no member has walks
     * all of it): a body cut short, or with junk after the fields, is refused. */
    if (vp_json_string(body, len, "\x01", NULL, 0) != VP_JSON_MISSING) {
        return false;
    }
    if (vp_json_string(body, len, "version", version, sizeof(version)) != VP_JSON_STRING ||
        !vo_version_parse(version, &m.v) ||
        vp_json_string(body, len, "sha256", sha, sizeof(sha)) != VP_JSON_STRING || !vo_sha_parse(sha, m.sha256) ||
        !vp_json_uint(body, len, "size", &m.size) || m.size == 0) {
        return false;
    }
    memcpy(m.version, version, strlen(version) + 1);
    memcpy(m.sha256_hex, sha, sizeof(m.sha256_hex));
    m.sha256_hex[2 * VO_SHA_LEN] = '\0';
    *out = m;
    return true;
}

bool vo_image_url(const vp_url_t *base, const vo_manifest_t *m, char *out, size_t cap)
{
    char suffix[16 + 2 * VO_SHA_LEN];
    uint8_t check[VO_SHA_LEN];
    if (!vo_sha_parse(m->sha256_hex, check) || memcmp(check, m->sha256, VO_SHA_LEN) != 0) {
        return false;
    }
    snprintf(suffix, sizeof(suffix), "/firmware/%s.bin", m->sha256_hex);
    return vp_url_join(base, suffix, out, cap);
}

bool vo_manifest_url(const vp_url_t *base, char *out, size_t cap)
{
    return vp_url_join(base, "/firmware/manifest", out, cap);
}

/* ---- Request headers ---- */

bool vo_headers(const char *token, const char *node_id, const char *credential, const char *running_version,
                char auth_buf[VP_AUTH_MAX], vp_header_t out[VO_HEADERS])
{
    vo_version_t v;
    memset(out, 0, sizeof(vp_header_t) * VO_HEADERS);
    auth_buf[0] = '\0';
    if (!vp_valid_token(token) || !vp_valid_node_id(node_id) || !vc_valid_credential(credential)) {
        return false;
    }
    if (!vo_version_parse(running_version, &v)) {
        running_version = "unknown";
    }
    snprintf(auth_buf, VP_AUTH_MAX, "Bearer %s", token);
    out[0] = (vp_header_t){ "Authorization", auth_buf };
    out[1] = (vp_header_t){ "X-Node-Id", node_id };
    out[2] = (vp_header_t){ VC_CRED_HEADER, credential };
    out[3] = (vp_header_t){ VO_FIRMWARE_HEADER, running_version };
    return true;
}

/* ---- What to do with a manifest response ---- */

vo_verdict_t vo_check_result(int status, const char *body, size_t len, const char *running, const char *rejected,
                             uint32_t slot_size, vo_manifest_t *out)
{
    memset(out, 0, sizeof(*out));
    if (status == 204) {
        return VO_UP_TO_DATE;
    }
    if (status == 401 || status == 403) {
        return VO_REFUSED;
    }
    if (status != 200) {
        return VO_FAILED;
    }
    if (!vo_manifest_parse(body, len, out)) {
        return VO_BAD_MANIFEST;
    }
    vo_version_t cur;
    if (!vo_version_parse(running, &cur)) {
        return VO_SKIP_UNVERSIONED;
    }
    int cmp = vo_version_cmp(&out->v, &cur);
    if (cmp == 0) {
        return VO_UP_TO_DATE;
    }
    if (cmp < 0) {
        return VO_SKIP_OLDER;
    }
    if (rejected && !strcmp(rejected, out->version)) {
        return VO_SKIP_REJECTED;
    }
    if (out->size > slot_size) {
        return VO_SKIP_TOO_BIG;
    }
    return VO_INSTALL;
}

bool vo_channel_ok(vo_verdict_t v)
{
    switch (v) {
    case VO_UP_TO_DATE:
    case VO_INSTALL:
    case VO_SKIP_OLDER:
    case VO_SKIP_REJECTED:
    case VO_SKIP_TOO_BIG:
    case VO_SKIP_UNVERSIONED:
        return true;
    case VO_BAD_MANIFEST:
    case VO_REFUSED:
    case VO_FAILED:
        return false;
    }
    return false;
}

const char *vo_verdict_name(vo_verdict_t v)
{
    switch (v) {
    case VO_UP_TO_DATE:
        return "up to date";
    case VO_INSTALL:
        return "newer version published";
    case VO_SKIP_OLDER:
        return "published version is older; not installed";
    case VO_SKIP_REJECTED:
        return "published version was rolled back here before; not installed";
    case VO_SKIP_TOO_BIG:
        return "published image is larger than the app slot; not installed";
    case VO_SKIP_UNVERSIONED:
        return "running version is not MAJOR.MINOR.PATCH; update over USB";
    case VO_BAD_MANIFEST:
        return "malformed manifest";
    case VO_REFUSED:
        return "refused (token or node credential)";
    case VO_FAILED:
        return "no answer";
    }
    return "?";
}

const char *vo_verdict_code(vo_verdict_t v)
{
    switch (v) {
    case VO_UP_TO_DATE:
        return "up_to_date";
    case VO_INSTALL:
        return "newer";
    case VO_SKIP_OLDER:
        return "older";
    case VO_SKIP_REJECTED:
        return "rolled_back";
    case VO_SKIP_TOO_BIG:
        return "too_big";
    case VO_SKIP_UNVERSIONED:
        return "unversioned";
    case VO_BAD_MANIFEST:
        return "bad_manifest";
    case VO_REFUSED:
        return "refused";
    case VO_FAILED:
        return "no_answer";
    }
    return "?";
}

/* ---- When to check ---- */

void vo_sched_init(vo_sched_t *s, int64_t now_ms)
{
    s->next_ms = now_ms + VO_FIRST_CHECK_MS;
    s->failures = 0;
}

bool vo_sched_due(const vo_sched_t *s, int64_t now_ms)
{
    return now_ms >= s->next_ms;
}

int64_t vo_sched_wait_ms(const vo_sched_t *s, int64_t now_ms)
{
    return now_ms >= s->next_ms ? 0 : s->next_ms - now_ms;
}

int64_t vo_jitter_ms(const char *node_id)
{
    uint32_t h = 2166136261u;   /* FNV-1a */
    for (const char *p = node_id ? node_id : ""; *p; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    return (int64_t)(h % (uint32_t)(VO_JITTER_MS / 1000)) * 1000;
}

void vo_sched_done(vo_sched_t *s, bool ok, const char *node_id, int64_t now_ms)
{
    if (ok) {
        s->failures = 0;
        s->next_ms = now_ms + VO_PERIOD_MS + vo_jitter_ms(node_id);
        return;
    }
    int64_t wait = VO_RETRY_MIN_MS;
    for (uint32_t i = 0; i < s->failures && wait < VO_PERIOD_MS; i++) {
        wait *= 2;
    }
    if (wait > VO_PERIOD_MS) {
        wait = VO_PERIOD_MS;
    }
    if (s->failures < UINT32_MAX) {
        s->failures++;
    }
    s->next_ms = now_ms + wait;
}

void vo_sched_now(vo_sched_t *s, int64_t now_ms)
{
    s->next_ms = now_ms;
}
