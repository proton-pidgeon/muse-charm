/*
 * Vesper node <-> backend wire protocol v1: pure-C core. See vesper_proto.h.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources.
 */
#include "vesper_proto.h"

#include <stdio.h>
#include <string.h>

/* ---- small helpers (no locale, no libc extensions) ---- */

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool starts_with_ci(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++) {
        if (lower(*s) != lower(*prefix)) {
            return false;
        }
    }
    return true;
}

static bool is_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static size_t bounded_strlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n]) {
        n++;
    }
    return n;
}

static void copy_str(char *out, size_t cap, const char *src)
{
    if (!cap) {
        return;
    }
    size_t n = bounded_strlen(src, cap - 1);
    memcpy(out, src, n);
    out[n] = '\0';
}

/* ---- UTF-8 ---- */

/* Length of the character starting at s[0] (1 for a stray or invalid byte). */
static size_t utf8_len(unsigned char c)
{
    if (c < 0x80) {
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

/* Whole characters of src[0..len) that fit in room bytes; stops at a NUL. */
static size_t utf8_fit(const char *src, size_t len, size_t room)
{
    size_t i = 0;
    while (i < len && src[i]) {
        size_t n = utf8_len((unsigned char)src[i]);
        if (n > 1) {
            /* a character cut off by the end of the input doesn't count */
            if (i + n > len) {
                break;
            }
            for (size_t k = 1; k < n; k++) {
                if (((unsigned char)src[i + k] & 0xC0) != 0x80) {
                    n = 1;   /* invalid sequence: take the lead byte alone */
                    break;
                }
            }
        }
        if (i + n > room) {
            break;
        }
        i += n;
    }
    return i;
}

size_t vp_utf8_copy(char *out, size_t cap, const char *src, size_t len)
{
    if (!cap) {
        return 0;
    }
    size_t n = utf8_fit(src, len, cap - 1);
    memcpy(out, src, n);
    out[n] = '\0';
    return n;
}

size_t vp_utf8_append(char *out, size_t cap, const char *src)
{
    size_t have = bounded_strlen(out, cap);
    if (have >= cap) {
        return 0;
    }
    size_t n = utf8_fit(src, strlen(src), cap - have - 1);
    /* Terminate the new end before copying over the old NUL, so a lock-free
     * reader on another task (muse_hatch_turn_caption) never finds the
     * buffer without a terminator. */
    out[have + n] = '\0';
    memcpy(out + have, src, n);
    return n;
}

/* Encodes code point cp; returns its length (1-4). */
static size_t utf8_put(uint32_t cp, char *o)
{
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xC0 | cp >> 6);
        o[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | cp >> 12);
        o[1] = (char)(0x80 | (cp >> 6 & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | cp >> 18);
    o[1] = (char)(0x80 | (cp >> 12 & 0x3F));
    o[2] = (char)(0x80 | (cp >> 6 & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ---- URLs ---- */

static bool url_path_char(char c)
{
    return is_alnum(c) || (c && strchr("-._~/!$&'()*+,;=:@%", c));
}

/* Parses base into out, or (out == NULL) only checks it, using no stack for
 * the parts: muse_hatch_configured() runs on small task stacks. */
static bool url_parse(const char *base, vp_url_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!base) {
        return false;
    }
    const char *p = base;
    bool https = false;
    if (starts_with_ci(p, "https://")) {
        https = true;
        p += 8;
    } else if (starts_with_ci(p, "http://")) {
        p += 7;
    } else {
        return false;
    }
    if (out) {
        out->https = https;
        out->port = https ? 443 : 80;
    }

    size_t hl = 0;
    const size_t host_cap = sizeof(((vp_url_t *)0)->host);
    if (*p == '[') {
        p++;
        while (*p && *p != ']') {
            char c = *p;
            if (!((c >= '0' && c <= '9') || (lower(c) >= 'a' && lower(c) <= 'f') || c == ':' || c == '.')) {
                return false;
            }
            if (hl + 1 >= host_cap) {
                return false;
            }
            if (out) {
                out->host[hl] = c;
            }
            hl++;
            p++;
        }
        if (*p != ']' || hl == 0) {
            return false;
        }
        p++;
        if (out) {
            out->ipv6 = true;
        }
    } else {
        while (*p && *p != '/' && *p != ':') {
            char c = *p;
            if (!(is_alnum(c) || c == '-' || c == '.')) {
                return false;   /* userinfo, query, spaces, ... */
            }
            if (hl + 1 >= host_cap) {
                return false;
            }
            if (out) {
                out->host[hl] = c;
            }
            hl++;
            p++;
        }
        if (hl == 0) {
            return false;
        }
    }

    if (*p == ':') {
        p++;
        unsigned long port = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (unsigned long)(*p - '0');
            if (port > 65535) {
                return false;
            }
            p++;
            digits++;
        }
        if (!digits || port == 0) {
            return false;
        }
        if (out) {
            out->port = (uint16_t)port;
        }
    }
    if (*p && *p != '/') {
        return false;
    }
    size_t pl = 0;
    const size_t path_cap = sizeof(((vp_url_t *)0)->path);
    for (; *p; p++) {
        if (!url_path_char(*p) || pl + 1 >= path_cap) {
            return false;
        }
        if (out) {
            out->path[pl] = *p;
        }
        pl++;
    }
    if (out) {
        while (pl && out->path[pl - 1] == '/') {
            pl--;
        }
        out->path[pl] = '\0';
    }
    return true;
}

bool vp_url_parse(const char *base, vp_url_t *out)
{
    return url_parse(base, out);
}

bool vp_url_valid(const char *base)
{
    return url_parse(base, NULL);
}

static int origin(const vp_url_t *u, char *out, size_t cap)
{
    bool default_port = u->port == (u->https ? 443 : 80);
    char port[8] = "";
    if (!default_port) {
        snprintf(port, sizeof(port), ":%u", (unsigned)u->port);
    }
    return snprintf(out, cap, "%s://%s%s%s%s", u->https ? "https" : "http", u->ipv6 ? "[" : "", u->host,
                    u->ipv6 ? "]" : "", port);
}

bool vp_url_join(const vp_url_t *u, const char *suffix, char *out, size_t cap)
{
    int n = origin(u, out, cap);
    if (n < 0 || (size_t)n >= cap) {
        return false;
    }
    int m = snprintf(out + n, cap - (size_t)n, "%s%s", u->path, suffix ? suffix : "");
    return m >= 0 && (size_t)m < cap - (size_t)n;
}

bool vp_resolve_audio_url(const vp_url_t *base, const char *ref, char *out, size_t cap)
{
    if (!ref || !ref[0]) {
        return false;
    }
    size_t len = bounded_strlen(ref, 161);
    if (len > 160) {
        return false;
    }
    /* Only path characters; no ':' (a scheme), '%', '\\', '?', '#', spaces or controls. */
    for (size_t i = 0; i < len; i++) {
        char c = ref[i];
        if (!(is_alnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/')) {
            return false;
        }
    }
    /* No empty, "." or ".." segments (which also rules out "//host"). */
    const char *seg = ref[0] == '/' ? ref + 1 : ref;
    for (;;) {
        const char *slash = strchr(seg, '/');
        size_t sl = slash ? (size_t)(slash - seg) : strlen(seg);
        if (sl == 0 || (sl == 1 && seg[0] == '.') || (sl == 2 && seg[0] == '.' && seg[1] == '.')) {
            return false;
        }
        if (!slash) {
            break;
        }
        seg = slash + 1;
    }
    if (ref[0] == '/') {
        /* absolute path: same origin, the base path doesn't apply */
        int n = origin(base, out, cap);
        if (n < 0 || (size_t)n >= cap) {
            return false;
        }
        int m = snprintf(out + n, cap - (size_t)n, "%s", ref);
        return m >= 0 && (size_t)m < cap - (size_t)n;
    }
    /* relative to <base>/turn: replaces the last segment ("turn") */
    char suffix[VP_URL_MAX];
    int m = snprintf(suffix, sizeof(suffix), "/%s", ref);
    if (m < 0 || (size_t)m >= sizeof(suffix)) {
        return false;
    }
    return vp_url_join(base, suffix, out, cap);
}

/* ---- Request ---- */

bool vp_valid_token(const char *token)
{
    if (!token) {
        return false;
    }
    size_t n = bounded_strlen(token, VP_TOKEN_MAX + 1);
    if (n == 0 || n > VP_TOKEN_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)token[i];
        if (c < 0x21 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

bool vp_valid_node_id(const char *node_id)
{
    if (!node_id) {
        return false;
    }
    size_t n = bounded_strlen(node_id, VP_NODE_ID_MAX + 1);
    if (n == 0 || n > VP_NODE_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = node_id[i];
        if (!(is_alnum(c) || c == '.' || c == '_' || c == ':' || c == '-')) {
            return false;
        }
    }
    return true;
}

bool vp_turn_headers(const char *token, const char *node_id, char auth_buf[VP_AUTH_MAX],
                     vp_header_t out[VP_TURN_HEADERS])
{
    auth_buf[0] = '\0';
    if (!vp_valid_token(token) || !vp_valid_node_id(node_id)) {
        return false;
    }
    snprintf(auth_buf, VP_AUTH_MAX, "Bearer %s", token);
    out[0] = (vp_header_t){ "Authorization", auth_buf };
    out[1] = (vp_header_t){ "X-Node-Id", node_id };
    out[2] = (vp_header_t){ "X-Vesper-Node-Protocol", VP_PROTOCOL_VERSION };
    out[3] = (vp_header_t){ "Content-Type", "audio/wav" };
    out[4] = (vp_header_t){ "Accept", "text/event-stream" };
    return true;
}

static void put_le(uint8_t *p, uint32_t v, int n)
{
    for (int i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

void vp_wav_header(uint8_t h[VP_WAV_HEADER], uint32_t rate)
{
    memcpy(h, "RIFF", 4);
    put_le(h + 4, UINT32_MAX, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le(h + 16, 16, 4);
    put_le(h + 20, 1, 2);             /* PCM */
    put_le(h + 22, 1, 2);             /* mono */
    put_le(h + 24, rate, 4);
    put_le(h + 28, rate * 2, 4);
    put_le(h + 32, 2, 2);
    put_le(h + 34, 16, 2);
    memcpy(h + 36, "data", 4);
    put_le(h + 40, UINT32_MAX, 4);
}

/* ---- Responses before the stream ---- */

static bool eq(const char *a, const char *b)
{
    return a && b && !strcmp(a, b);
}

vp_http_verdict_t vp_http_verdict(int status, const char *content_type, const char *error_code,
                                  const char *retry_after, int attempt, int *retry_ms,
                                  char *caption, size_t cap)
{
    const char *msg;
    if (retry_ms) {
        *retry_ms = 0;
    }
    if (status == 200) {
        if (content_type && starts_with_ci(content_type, "text/event-stream")) {
            copy_str(caption, cap, "");
            return VP_HTTP_STREAM;
        }
        msg = "BAD REPLY FROM VESPER";
    } else if (status == 503) {
        if (attempt == 0) {
            long secs = 2;
            if (retry_after && retry_after[0] >= '0' && retry_after[0] <= '9') {
                secs = 0;
                for (const char *p = retry_after; *p >= '0' && *p <= '9' && secs < 100; p++) {
                    secs = secs * 10 + (*p - '0');
                }
            }
            secs = secs < 1 ? 1 : secs > 5 ? 5 : secs;   /* don't keep a press waiting long */
            if (retry_ms) {
                *retry_ms = (int)(secs * 1000);
            }
            copy_str(caption, cap, "VESPER IS BUSY - RETRYING");
            return VP_HTTP_RETRY;
        }
        msg = "VESPER IS BUSY";
    } else if (status == 401) {
        msg = "TOKEN REFUSED";
    } else if (status == 403) {
        /* task 08/11: node_unauthorized sends the node back to the claim flow */
        msg = eq(error_code, "node_unauthorized") ? "NODE NOT CLAIMED" : "REQUEST REFUSED";
    } else if (status == 400) {
        msg = eq(error_code, "unsupported_protocol") ? "UPDATE THE FIRMWARE"
              : eq(error_code, "bad_node_id")         ? "BAD DEVICE ID"
                                                      : "REQUEST REFUSED";
    } else if (status == 413) {
        msg = "NOTE TOO LONG";
    } else if (status == 415) {
        msg = "BAD AUDIO TYPE";
    } else if (status == 422) {
        msg = "COULDN'T READ THE AUDIO";
    } else if (status == 408) {
        msg = "UPLOAD TOO SLOW";
    } else if (status == 404 || status == 405) {
        msg = "CHECK THE SERVER URL";
    } else if (status <= 0) {
        msg = "CAN'T REACH VESPER";
    } else {
        msg = "VESPER SERVER ERROR";
    }
    copy_str(caption, cap, msg);
    return VP_HTTP_FAIL;
}

void vp_error_caption(const char *code, const char *message, char *out, size_t cap)
{
    if (message && message[0]) {
        vp_utf8_copy(out, cap, message, strlen(message));
        return;
    }
    const char *msg = eq(code, "empty_transcript")      ? "DIDN'T CATCH THAT"
                      : eq(code, "transcript_too_long") ? "TOO LONG - TRY SHORTER"
                      : eq(code, "stt_failed")          ? "COULDN'T TRANSCRIBE"
                      : eq(code, "ask_failed")          ? "VESPER DIDN'T ANSWER"
                                                        : "SOMETHING WENT WRONG";
    copy_str(out, cap, msg);
}

/* ---- Minimal JSON ---- */

#define JSON_DEPTH_MAX 16

typedef struct {
    const char *p, *end;
} jcur_t;

static void jws(jcur_t *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) {
        c->p++;
    }
}

static int hexval(char h)
{
    if (h >= '0' && h <= '9') {
        return h - '0';
    }
    h = lower(h);
    if (h >= 'a' && h <= 'f') {
        return h - 'a' + 10;
    }
    return -1;
}

static bool read_u16(jcur_t *c, uint32_t *v)
{
    if (c->end - c->p < 4) {
        return false;
    }
    uint32_t x = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval(c->p[i]);
        if (h < 0) {
            return false;
        }
        x = x << 4 | (uint32_t)h;
    }
    c->p += 4;
    *v = x;
    return true;
}

/*
 * Decodes the string at c->p (which is at the opening quote) into out (if
 * not NULL), UTF-8-safely cut to cap. Once a character doesn't fit, nothing
 * more is written, so the result is always a prefix of the string. False on
 * malformed input.
 */
static bool jstring(jcur_t *c, char *out, size_t cap, bool *cut)
{
    size_t o = 0;
    bool full = out == NULL || cap == 0;
    if (cut) {
        *cut = false;
    }
    if (c->p >= c->end || *c->p != '"') {
        return false;
    }
    c->p++;
    while (c->p < c->end) {
        unsigned char ch = (unsigned char)*c->p;
        char buf[4];
        size_t bl;
        if (ch == '"') {
            c->p++;
            if (out && cap) {
                out[o] = '\0';
            }
            return true;
        }
        if (ch < 0x20) {
            return false;   /* raw control character */
        }
        if (ch == '\\') {
            c->p++;
            if (c->p >= c->end) {
                return false;
            }
            char e = *c->p++;
            uint32_t cp;
            switch (e) {
            case '"': buf[0] = '"'; bl = 1; break;
            case '\\': buf[0] = '\\'; bl = 1; break;
            case '/': buf[0] = '/'; bl = 1; break;
            case 'b': buf[0] = '\b'; bl = 1; break;
            case 'f': buf[0] = '\f'; bl = 1; break;
            case 'n': buf[0] = '\n'; bl = 1; break;
            case 'r': buf[0] = '\r'; bl = 1; break;
            case 't': buf[0] = '\t'; bl = 1; break;
            case 'u':
                if (!read_u16(c, &cp)) {
                    return false;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t lo;
                    jcur_t save = *c;
                    if (c->end - c->p >= 6 && c->p[0] == '\\' && c->p[1] == 'u') {
                        c->p += 2;
                        if (read_u16(c, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            *c = save;   /* lone high surrogate: the next escape stands alone */
                            cp = 0xFFFD;
                        }
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;   /* lone low surrogate */
                } else if (cp == 0) {
                    cp = 0xFFFD;   /* no NULs inside C strings */
                }
                bl = utf8_put(cp, buf);
                break;
            default:
                return false;
            }
        } else {
            /* a raw UTF-8 character, copied whole */
            size_t n = utf8_len(ch);
            if (n > 1) {
                if ((size_t)(c->end - c->p) < n) {
                    return false;
                }
                for (size_t k = 1; k < n; k++) {
                    if (((unsigned char)c->p[k] & 0xC0) != 0x80) {
                        n = 1;
                        break;
                    }
                }
            }
            memcpy(buf, c->p, n);
            bl = n;
            c->p += n;
        }
        if (!full) {
            if (o + bl <= cap - 1) {
                memcpy(out + o, buf, bl);
                o += bl;
            } else {
                full = true;
                if (cut) {
                    *cut = true;
                }
            }
        }
    }
    return false;   /* unterminated */
}

/* Skips one value of any type, without recursion. */
static bool jskip(jcur_t *c)
{
    jws(c);
    if (c->p >= c->end) {
        return false;
    }
    char ch = *c->p;
    if (ch == '"') {
        return jstring(c, NULL, 0, NULL);
    }
    if (ch == '{' || ch == '[') {
        int depth = 0;
        while (c->p < c->end) {
            ch = *c->p;
            if (ch == '"') {
                if (!jstring(c, NULL, 0, NULL)) {
                    return false;
                }
                continue;
            }
            if (ch == '{' || ch == '[') {
                if (++depth > JSON_DEPTH_MAX) {
                    return false;
                }
            } else if (ch == '}' || ch == ']') {
                if (--depth == 0) {
                    c->p++;
                    return true;
                }
            }
            c->p++;
        }
        return false;
    }
    /* number or literal */
    const char *start = c->p;
    while (c->p < c->end && (is_alnum(*c->p) || *c->p == '-' || *c->p == '+' || *c->p == '.')) {
        c->p++;
    }
    return c->p > start;
}

/*
 * Walks the top-level members; at `key` leaves the cursor on its value and
 * returns VP_JSON_OTHER (the caller looks at the value), else MISSING or BAD.
 */
static vp_json_kind_t jfind(jcur_t *c, const char *key)
{
    jws(c);
    if (c->p >= c->end || *c->p != '{') {
        return VP_JSON_BAD;
    }
    c->p++;
    jws(c);
    if (c->p < c->end && *c->p == '}') {
        return VP_JSON_MISSING;
    }
    for (;;) {
        char name[48];
        bool cut;
        jws(c);
        if (!jstring(c, name, sizeof(name), &cut)) {
            return VP_JSON_BAD;
        }
        jws(c);
        if (c->p >= c->end || *c->p != ':') {
            return VP_JSON_BAD;
        }
        c->p++;
        jws(c);
        if (!cut && !strcmp(name, key)) {
            return VP_JSON_OTHER;
        }
        if (!jskip(c)) {
            return VP_JSON_BAD;
        }
        jws(c);
        if (c->p >= c->end) {
            return VP_JSON_BAD;
        }
        if (*c->p == '}') {
            return VP_JSON_MISSING;
        }
        if (*c->p != ',') {
            return VP_JSON_BAD;
        }
        c->p++;
    }
}

vp_json_kind_t vp_json_string(const char *json, size_t len, const char *key, char *out, size_t cap)
{
    if (out && cap) {
        out[0] = '\0';
    }
    jcur_t c = { json, json + len };
    vp_json_kind_t k = jfind(&c, key);
    if (k != VP_JSON_OTHER) {
        return k;
    }
    if (c.p < c.end && *c.p == '"') {
        if (!jstring(&c, out, cap, NULL)) {
            if (out && cap) {
                out[0] = '\0';
            }
            return VP_JSON_BAD;
        }
        return VP_JSON_STRING;
    }
    if (c.end - c.p >= 4 && !memcmp(c.p, "null", 4)) {
        return VP_JSON_NULL;
    }
    return VP_JSON_OTHER;
}

bool vp_json_bool(const char *json, size_t len, const char *key, bool *found)
{
    jcur_t c = { json, json + len };
    *found = false;
    if (jfind(&c, key) != VP_JSON_OTHER) {
        return false;
    }
    if (c.end - c.p >= 4 && !memcmp(c.p, "true", 4)) {
        *found = true;
        return true;
    }
    if (c.end - c.p >= 5 && !memcmp(c.p, "false", 5)) {
        *found = true;
    }
    return false;
}

bool vp_json_uint(const char *json, size_t len, const char *key, uint32_t *out)
{
    jcur_t c = { json, json + len };
    *out = 0;
    if (jfind(&c, key) != VP_JSON_OTHER) {
        return false;
    }
    uint64_t v = 0;
    const char *start = c.p;
    while (c.p < c.end && *c.p >= '0' && *c.p <= '9') {
        v = v * 10 + (uint64_t)(*c.p - '0');
        if (v > UINT32_MAX) {
            return false;
        }
        c.p++;
    }
    if (c.p == start || (c.p - start > 1 && *start == '0')) {
        return false;   /* no digits, or a leading zero (not JSON) */
    }
    if (c.p < c.end && (is_alnum(*c.p) || *c.p == '.' || *c.p == '-' || *c.p == '+')) {
        return false;   /* a fraction, an exponent or junk */
    }
    *out = (uint32_t)v;
    return true;
}

/* ---- SSE framing ---- */

static bool s_line_started(const vp_sse_t *s)
{
    return s->line_len || s->line_skip;
}

void vp_sse_init(vp_sse_t *s, vp_sse_event_cb cb, void *ctx)
{
    memset(s, 0, sizeof(*s));
    s->cb = cb;
    s->ctx = ctx;
}

static void reset_event(vp_sse_t *s)
{
    s->event[0] = '\0';
    s->data[0] = '\0';
    s->data_len = 0;
    s->have_data = false;
    s->broken = false;
}

static void dispatch(vp_sse_t *s)
{
    if (s->broken) {
        if (s->have_data || s->event[0]) {
            s->dropped++;
        }
    } else if (s->have_data && s->cb) {
        s->data[s->data_len] = '\0';
        s->cb(s->ctx, s->event[0] ? s->event : "message", s->data, s->data_len);
    }
    reset_event(s);
}

static void end_line(vp_sse_t *s)
{
    if (!s_line_started(s)) {
        dispatch(s);   /* blank line */
        return;
    }
    if (s->line_skip) {
        if (s->line_oversized) {
            s->broken = true;   /* a field of this event was lost */
        }
    } else {
        s->line[s->line_len] = '\0';
        char *colon = memchr(s->line, ':', s->line_len);
        const char *value = "";
        size_t vlen = 0;
        if (colon) {
            *colon = '\0';
            value = colon + 1;
            if (*value == ' ') {
                value++;
            }
            vlen = s->line_len - (size_t)(value - s->line);
        }
        if (!strcmp(s->line, "event")) {
            if (vlen >= sizeof(s->event)) {
                /* no event of ours is this long: an unknown event either way */
                memcpy(s->event, "?", 2);
            } else {
                memcpy(s->event, value, vlen);
                s->event[vlen] = '\0';
            }
        } else if (!strcmp(s->line, "data")) {
            size_t need = vlen + (s->have_data ? 1 : 0);
            if (s->data_len + need >= sizeof(s->data)) {
                s->broken = true;
            } else {
                if (s->have_data) {
                    s->data[s->data_len++] = '\n';
                }
                memcpy(s->data + s->data_len, value, vlen);
                s->data_len += vlen;
                s->have_data = true;
            }
        }
        /* id, retry and unknown fields are ignored */
    }
    s->line_len = 0;
    s->line_skip = false;
    s->line_oversized = false;
}

void vp_sse_feed(vp_sse_t *s, const char *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];
        if (s->after_cr) {
            s->after_cr = false;
            if (c == '\n') {
                continue;   /* the LF of a CRLF */
            }
        }
        if (c == '\r' || c == '\n') {
            end_line(s);
            s->after_cr = c == '\r';
            continue;
        }
        if (s->line_skip) {
            continue;
        }
        if (!s->line_len && c == ':') {
            s->line_skip = true;   /* a comment: the preamble or a ping */
            continue;
        }
        if (s->line_len + 1 >= sizeof(s->line)) {
            s->line_skip = true;
            s->line_oversized = true;
            continue;
        }
        s->line[s->line_len++] = c;
    }
}

void vp_sse_finish(vp_sse_t *s)
{
    s->line_len = 0;
    s->line_skip = false;
    s->line_oversized = false;
    s->after_cr = false;
    reset_event(s);
}

/* ---- A turn's events ---- */

static void turn_event(void *ctx, const char *event, const char *data, size_t len)
{
    vp_turn_t *t = ctx;
    const vp_turn_cbs_t *cb = t->cbs;
    if (t->done) {
        return;
    }
    vp_json_kind_t k;
    if (!strcmp(event, "transcript")) {
        k = vp_json_string(data, len, "text", t->scratch, sizeof(t->scratch));
        if (k == VP_JSON_STRING) {
            if (cb->transcript) {
                cb->transcript(t->ctx, t->scratch);
            }
        } else if (k == VP_JSON_BAD) {
            t->bad_events++;
        }
    } else if (!strcmp(event, "message_start")) {
        k = vp_json_string(data, len, "id", t->msg_id, sizeof(t->msg_id));
        if (k == VP_JSON_BAD) {
            t->bad_events++;
            return;
        }
        if (k != VP_JSON_STRING) {
            snprintf(t->msg_id, sizeof(t->msg_id), "m%u", t->messages + 1);
        }
        t->in_message = true;
        t->messages++;
        if (cb->message_start) {
            cb->message_start(t->ctx, t->msg_id);
        }
    } else if (!strcmp(event, "text_delta")) {
        k = vp_json_string(data, len, "text", t->scratch, sizeof(t->scratch));
        if (k != VP_JSON_STRING) {
            t->bad_events += k == VP_JSON_BAD;
            return;
        }
        if (!t->in_message) {
            /* a delta without its start: open the message it names */
            if (vp_json_string(data, len, "id", t->msg_id, sizeof(t->msg_id)) != VP_JSON_STRING) {
                snprintf(t->msg_id, sizeof(t->msg_id), "m%u", t->messages + 1);
            }
            t->in_message = true;
            t->messages++;
            if (cb->message_start) {
                cb->message_start(t->ctx, t->msg_id);
            }
        }
        if (cb->text_delta && t->scratch[0]) {
            cb->text_delta(t->ctx, t->msg_id, t->scratch);
        }
    } else if (!strcmp(event, "message_done")) {
        k = vp_json_string(data, len, "audio_url", t->scratch, sizeof(t->scratch));
        if (k == VP_JSON_BAD) {
            t->bad_events++;
        }
        const char *url = NULL;
        if (k == VP_JSON_STRING && t->base &&
            vp_resolve_audio_url(t->base, t->scratch, t->scratch2, sizeof(t->scratch2))) {
            url = t->scratch2;
        }
        if (!t->in_message) {
            return;   /* nothing was said */
        }
        t->in_message = false;
        if (cb->message_done) {
            cb->message_done(t->ctx, t->msg_id, url);
        }
    } else if (!strcmp(event, "error")) {
        char code[VP_CODE_MAX];
        if (vp_json_string(data, len, "code", code, sizeof(code)) != VP_JSON_STRING) {
            code[0] = '\0';
        }
        if (vp_json_string(data, len, "message", t->scratch, sizeof(t->scratch)) != VP_JSON_STRING) {
            t->scratch[0] = '\0';
        }
        vp_error_caption(code, t->scratch, t->scratch2, sizeof(t->scratch2));
        t->errored = true;
        if (cb->error) {
            cb->error(t->ctx, code, t->scratch2);
        }
    } else if (!strcmp(event, "done")) {
        bool found;
        bool ok = vp_json_bool(data, len, "ok", &found);
        t->done = true;
        if (cb->done) {
            cb->done(t->ctx, found && ok && !t->errored);
        }
    } else if (!strcmp(event, "timing")) {
        /* diagnostics only */
    } else {
        t->unknown_events++;   /* compatible additions: ignored */
    }
}

void vp_turn_init(vp_turn_t *t, const vp_url_t *base, const vp_turn_cbs_t *cbs, void *ctx)
{
    memset(t, 0, sizeof(*t));
    t->cbs = cbs;
    t->ctx = ctx;
    t->base = base;
    vp_sse_init(&t->sse, turn_event, t);
}

void vp_turn_feed(vp_turn_t *t, const char *buf, size_t n)
{
    if (!t->done) {
        vp_sse_feed(&t->sse, buf, n);
    }
}

bool vp_turn_finish(vp_turn_t *t)
{
    vp_sse_finish(&t->sse);
    return t->done;
}
