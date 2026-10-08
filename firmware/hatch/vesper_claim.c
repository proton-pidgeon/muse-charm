/*
 * The node side of the claim flow: pure-C core. See vesper_claim.h.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources.
 */
#include "vesper_claim.h"

#include <stdio.h>
#include <string.h>

/* ---- helpers ---- */

static size_t bounded_len(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n]) {
        n++;
    }
    return n;
}

static bool urlsafe(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static bool alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/* Volatile stores, so wiping a secret isn't optimised away. */
static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--) {
        *v++ = 0;
    }
}

static void put(char *out, size_t cap, const char *s)
{
    if (cap) {
        snprintf(out, cap, "%s", s);
    }
}

/* prefix + 16..max-strlen(prefix) url-safe characters */
static bool valid_prefixed(const char *s, const char *prefix, size_t max)
{
    if (!s) {
        return false;
    }
    size_t pl = strlen(prefix);
    size_t n = bounded_len(s, max + 1);
    if (n > max || n < pl + 16 || strncmp(s, prefix, pl) != 0) {
        return false;
    }
    for (size_t i = pl; i < n; i++) {
        if (!urlsafe(s[i])) {
            return false;
        }
    }
    return true;
}

/* ---- validation ---- */

bool vc_valid_code(const char *code)
{
    if (!code || bounded_len(code, VC_CODE_LEN + 1) != VC_CODE_LEN) {
        return false;
    }
    for (int i = 0; i < VC_CODE_LEN; i++) {
        if (i == 4) {
            if (code[i] != '-') {
                return false;
            }
        } else if (!code[i] || !strchr(VC_CODE_ALPHABET, code[i])) {
            return false;
        }
    }
    return true;
}

bool vc_valid_secret(const char *secret)
{
    return valid_prefixed(secret, VC_SECRET_PREFIX, VC_SECRET_MAX);
}

bool vc_valid_credential(const char *credential)
{
    return valid_prefixed(credential, VC_CRED_PREFIX, VC_CRED_MAX);
}

bool vc_valid_room(const char *room)
{
    if (!room) {
        return false;
    }
    size_t n = bounded_len(room, VC_ROOM_MAX + 1);
    if (n == 0 || n > VC_ROOM_MAX || !alnum(room[0]) || !alnum(room[n - 1])) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!alnum(room[i]) && room[i] != ' ' && room[i] != '-' && room[i] != '_') {
            return false;
        }
    }
    return true;
}

/* ---- headers ---- */

int vc_claim_headers(const char *token, const char *node_id, const char *secret, char auth_buf[VP_AUTH_MAX],
                     vp_header_t out[VC_CLAIM_HEADERS])
{
    if (!vp_valid_token(token) || !vp_valid_node_id(node_id) || (secret && !vc_valid_secret(secret))) {
        return 0;
    }
    snprintf(auth_buf, VP_AUTH_MAX, "Bearer %s", token);
    out[0] = (vp_header_t){ "Authorization", auth_buf };
    out[1] = (vp_header_t){ "X-Node-Id", node_id };
    out[2] = (vp_header_t){ "X-Vesper-Node-Protocol", VP_PROTOCOL_VERSION };
    out[3] = (vp_header_t){ "Accept", "application/json" };
    if (!secret) {
        return 4;
    }
    out[4] = (vp_header_t){ VC_SECRET_HEADER, secret };
    return 5;
}

bool vc_credential_header(const char *credential, vp_header_t *out)
{
    if (!vc_valid_credential(credential)) {
        return false;
    }
    *out = (vp_header_t){ VC_CRED_HEADER, credential };
    return true;
}

bool vc_needs_claim(int status, const char *error_code)
{
    return status == 403 && error_code && !strcmp(error_code, "node_unauthorized");
}

int vc_retry_after_s(const char *retry_after, int dflt)
{
    long secs = -1;
    if (retry_after) {
        const char *p = retry_after;
        while (*p == ' ') {
            p++;
        }
        if (*p >= '0' && *p <= '9') {
            secs = 0;
            for (; *p >= '0' && *p <= '9'; p++) {
                if (secs <= VC_RETRY_AFTER_MAX_S) {
                    secs = secs * 10 + (*p - '0');
                }
            }
        }
    }
    if (secs < 0) {
        secs = dflt;
    }
    return secs < 1 ? 1 : secs > VC_RETRY_AFTER_MAX_S ? VC_RETRY_AFTER_MAX_S : (int)secs;
}

/* ---- state machine ---- */

static void drop_code(vc_claim_t *c)
{
    wipe(c->secret, sizeof(c->secret));
    c->code[0] = '\0';
    c->polls = 0;
}

void vc_init(vc_claim_t *c, bool have_credential, int64_t now_ms)
{
    wipe(c, sizeof(*c));
    c->state = have_credential ? VC_CLAIMED : VC_START;
    c->due_ms = now_ms;
    c->backoff_ms = VC_BACKOFF_MIN_MS;
}

vc_action_t vc_due(const vc_claim_t *c, int64_t now_ms)
{
    if (c->state == VC_CLAIMED || now_ms < c->due_ms) {
        return VC_ACT_NONE;
    }
    return c->state == VC_START ? VC_ACT_START : VC_ACT_POLL;
}

int64_t vc_wait_ms(const vc_claim_t *c, int64_t now_ms)
{
    if (c->state == VC_CLAIMED) {
        return -1;
    }
    return c->due_ms > now_ms ? c->due_ms - now_ms : 0;
}

/* The earliest a new /claim/start is accepted (the backend's 5 s restart interval). */
static int64_t start_due(const vc_claim_t *c, int64_t now_ms)
{
    int64_t earliest = c->started ? c->last_start_ms + VC_RESTART_MS : now_ms;
    return earliest > now_ms ? earliest : now_ms;
}

static void back_off(vc_claim_t *c, vc_note_t note, int64_t now_ms)
{
    c->note = note;
    c->due_ms = now_ms + c->backoff_ms;
    c->backoff_ms = c->backoff_ms * 2 > VC_BACKOFF_MAX_MS ? VC_BACKOFF_MAX_MS : c->backoff_ms * 2;
}

static bool retry_after_given(const char *ra)
{
    if (!ra) {
        return false;
    }
    while (*ra == ' ') {
        ra++;
    }
    return *ra >= '0' && *ra <= '9';
}

/* A failure that isn't about this claim in particular (both start and poll). */
static vc_event_t problem(vc_claim_t *c, int status, const char *retry_after, int busy_dflt_s, int64_t now_ms)
{
    if (status == 429 || (status == 503 && retry_after_given(retry_after))) {
        c->note = VC_NOTE_BUSY;
        c->due_ms = now_ms + (int64_t)vc_retry_after_s(retry_after, busy_dflt_s) * 1000;
    } else if (status == 503) {
        back_off(c, VC_NOTE_BUSY, now_ms);   /* registry_unavailable: no Retry-After */
    } else if (status == 401) {
        c->note = VC_NOTE_TOKEN_REFUSED;
        c->due_ms = now_ms + VC_REFUSED_MS;
    } else if (status == 400) {
        c->note = VC_NOTE_REFUSED;
        c->due_ms = now_ms + VC_REFUSED_MS;
    } else if (status <= 0) {
        back_off(c, VC_NOTE_UNREACHABLE, now_ms);
    } else {
        back_off(c, VC_NOTE_SERVER_ERROR, now_ms);
    }
    return VC_EV_PROBLEM;
}

vc_event_t vc_start_result(vc_claim_t *c, int status, const char *body, size_t len, const char *retry_after,
                           int64_t now_ms)
{
    if (c->state != VC_START) {
        return VC_EV_NONE;
    }
    if (status > 0) {
        /* The backend has seen a start (even a refused one counts toward its rate limit). */
        c->last_start_ms = now_ms;
        c->started = true;
    }
    if (status == 200) {
        char code[VC_CODE_LEN + 2];
        char secret[VC_SECRET_MAX + 2];   /* one more than fits: a longer one is cut and fails the check */
        char st[16];
        bool ok = body && vp_json_string(body, len, "status", st, sizeof(st)) == VP_JSON_STRING &&
                  !strcmp(st, "pending") &&
                  vp_json_string(body, len, "claim_code", code, sizeof(code)) == VP_JSON_STRING &&
                  vc_valid_code(code) &&
                  vp_json_string(body, len, "claim_secret", secret, sizeof(secret)) == VP_JSON_STRING &&
                  vc_valid_secret(secret);
        if (!ok) {
            wipe(secret, sizeof(secret));
            return problem(c, 500, NULL, 0, now_ms);
        }
        put(c->code, sizeof(c->code), code);
        put(c->secret, sizeof(c->secret), secret);
        wipe(secret, sizeof(secret));
        c->state = VC_POLL;
        c->note = VC_NOTE_NONE;
        c->polls = 0;
        c->codes++;
        c->backoff_ms = VC_BACKOFF_MIN_MS;
        c->due_ms = now_ms + VC_POLL_MS;
        return VC_EV_CODE;
    }
    if (status == 404 || status == 405) {
        c->note = VC_NOTE_NO_CLAIM_ROUTE;
        c->due_ms = now_ms + VC_REFUSED_MS;
        return VC_EV_PROBLEM;
    }
    /* rate_limited (Retry-After: 5) or too_many_pending (Retry-After: 60) */
    return problem(c, status, retry_after, 5, now_ms);
}

vc_event_t vc_poll_result(vc_claim_t *c, int status, const char *body, size_t len, const char *retry_after,
                          int64_t now_ms)
{
    if (c->state != VC_POLL) {
        return VC_EV_NONE;
    }
    if (status == 202) {
        c->polls++;
        c->note = VC_NOTE_NONE;
        c->backoff_ms = VC_BACKOFF_MIN_MS;
        c->due_ms = now_ms + VC_POLL_MS;
        return VC_EV_PENDING;
    }
    if (status == 200) {
        char cred[VC_CRED_MAX + 2];
        char room[VC_ROOM_MAX + 2];
        char st[16];
        bool ok = body && vp_json_string(body, len, "status", st, sizeof(st)) == VP_JSON_STRING &&
                  !strcmp(st, "claimed") &&
                  vp_json_string(body, len, "credential", cred, sizeof(cred)) == VP_JSON_STRING &&
                  vc_valid_credential(cred);
        if (ok) {
            if (vp_json_string(body, len, "room", room, sizeof(room)) != VP_JSON_STRING || !vc_valid_room(room)) {
                room[0] = '\0';   /* for the screen only; the backend keeps the room */
            }
            put(c->credential, sizeof(c->credential), cred);
            wipe(cred, sizeof(cred));
            put(c->room, sizeof(c->room), room);
            drop_code(c);
            c->state = VC_CLAIMED;
            c->note = VC_NOTE_NONE;
            c->backoff_ms = VC_BACKOFF_MIN_MS;
            return VC_EV_CLAIMED;
        }
        wipe(cred, sizeof(cred));
        /* The one delivery is lost (the backend only sends it once): start over. */
    }
    if (status == 200 || status == 404) {
        drop_code(c);
        c->state = VC_START;
        c->note = status == 404 ? VC_NOTE_EXPIRED : VC_NOTE_SERVER_ERROR;
        c->due_ms = start_due(c, now_ms);
        return VC_EV_RESTART;
    }
    /* 429/503/401/400/no response/5xx: the secret is still good, keep polling later */
    return problem(c, status, retry_after, 3, now_ms);
}

void vc_reclaim(vc_claim_t *c, int64_t now_ms)
{
    if (c->state != VC_CLAIMED) {
        return;   /* already claiming */
    }
    wipe(c->credential, sizeof(c->credential));
    c->room[0] = '\0';
    drop_code(c);
    c->state = VC_START;
    c->note = VC_NOTE_NONE;
    c->backoff_ms = VC_BACKOFF_MIN_MS;
    c->due_ms = start_due(c, now_ms);
}

void vc_config_changed(vc_claim_t *c, int64_t now_ms)
{
    if (c->state == VC_CLAIMED) {
        return;
    }
    drop_code(c);
    c->state = VC_START;
    c->note = VC_NOTE_NONE;
    c->backoff_ms = VC_BACKOFF_MIN_MS;
    c->due_ms = start_due(c, now_ms);
}

bool vc_take_credential(vc_claim_t *c, char out[VC_CRED_MAX + 1])
{
    if (!c->credential[0]) {
        return false;
    }
    memcpy(out, c->credential, VC_CRED_MAX + 1);
    wipe(c->credential, sizeof(c->credential));
    return true;
}

void vc_wipe(vc_claim_t *c)
{
    wipe(c->secret, sizeof(c->secret));
    wipe(c->credential, sizeof(c->credential));
}

void vc_caption(const vc_claim_t *c, char *out, size_t cap)
{
    if (c->state == VC_CLAIMED) {
        put(out, cap, "");
        return;
    }
    if (c->state == VC_POLL && c->code[0]) {
        /* A problem while polling doesn't hide the code: it may still be approved. */
        snprintf(out, cap, "CLAIM CODE %s", c->code);
        return;
    }
    const char *msg;
    switch (c->note) {
    case VC_NOTE_UNREACHABLE: msg = "CAN'T REACH VESPER"; break;
    case VC_NOTE_TOKEN_REFUSED: msg = "TOKEN REFUSED"; break;
    case VC_NOTE_REFUSED: msg = "CLAIM REFUSED"; break;
    case VC_NOTE_NO_CLAIM_ROUTE: msg = "CHECK THE SERVER URL"; break;
    case VC_NOTE_BUSY: msg = "VESPER IS BUSY"; break;
    case VC_NOTE_SERVER_ERROR: msg = "VESPER SERVER ERROR"; break;
    case VC_NOTE_EXPIRED: msg = "GETTING A NEW CODE"; break;
    case VC_NOTE_NONE:
    default: msg = "GETTING A CLAIM CODE"; break;
    }
    put(out, cap, msg);
}

const char *vc_state_name(const vc_claim_t *c)
{
    switch (c->state) {
    case VC_CLAIMED: return "claimed";
    case VC_POLL: return "pending";
    case VC_START:
    default: return "starting";
    }
}
