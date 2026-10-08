/*
 * Vesper node <-> backend wire protocol v1: the pure-C core of the firmware's
 * third muse_hatch_* backend (muse_chat_vesper.c).
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources.
 *
 * Spec: docs/node-wire-protocol.md in the muse-charm repo. This file has no
 * ESP-IDF, FreeRTOS or libc-extension dependencies (only <stddef.h>,
 * <stdint.h>, <stdbool.h>, <string.h>, <stdio.h>), so the same code runs on
 * the ESP32-S3 and in the host tests (firmware/hatch/test/).
 *
 * Everything here is bounded: fixed-size buffers, no allocation, no
 * recursion. Over-long input is cut (on a UTF-8 character boundary) or
 * skipped, never overflowed.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VP_PROTOCOL_VERSION "1"

/* ---- Sizes ---- */

#define VP_URL_MAX 192          /* a base URL or a resolved URL, with the NUL */
#define VP_TOKEN_MAX 1023       /* matches MUSE_TOKEN_MAX */
#define VP_NODE_ID_MAX 128      /* protocol: ^[A-Za-z0-9._:-]{1,128}$ */
#define VP_AUTH_MAX (7 + VP_TOKEN_MAX + 1)   /* "Bearer " + token + NUL */
#define VP_NOTE_MAX_BYTES 524288 /* protocol body cap (512 KiB) */
#define VP_WAV_HEADER 44

/* SSE: one line (field name + value) and one event's data. A v1 text_delta
 * carries the whole reply (/ask caps it at a few sentences, the backend's TTS
 * at 500 chars); 4 KiB leaves room for \uXXXX escapes. A longer line is
 * skipped, and the event it belonged to is dropped. Comment lines (": ...",
 * the 2 KiB priming preamble, pings) are skipped without being buffered, so
 * their length doesn't matter. */
#define VP_SSE_LINE_MAX 4096
#define VP_SSE_EVENT_MAX 32
#define VP_SSE_DATA_MAX 4096

#define VP_TEXT_MAX 1024        /* one decoded JSON string field */
#define VP_ID_MAX 64            /* message id ("m1") */
#define VP_CODE_MAX 40          /* error code */

/* ---- UTF-8 ---- */

/*
 * Copies up to cap - 1 bytes of src[0..len) into out (always NUL-terminated
 * when cap > 0), never splitting a UTF-8 character: a character that doesn't
 * fit whole is left out, with everything after it. Returns the bytes copied.
 */
size_t vp_utf8_copy(char *out, size_t cap, const char *src, size_t len);

/* Appends src to the NUL-terminated out (capacity cap) the same way. Returns
 * the number of bytes appended; fewer than strlen(src) means it was cut. */
size_t vp_utf8_append(char *out, size_t cap, const char *src);

/* ---- URLs ---- */

typedef struct {
    bool https;
    char host[96];      /* without brackets for IPv6 literals */
    bool ipv6;
    uint16_t port;      /* explicit or the scheme's default */
    char path[VP_URL_MAX];   /* the base path without a trailing slash; "" for root */
} vp_url_t;

/*
 * Parses the configured server base URL (NVS muse:host), e.g.
 * "https://peggy.fly.dev/vesper-node" or "http://[::1]:8796". Only http and
 * https, no userinfo, query or fragment, no whitespace or control characters.
 */
bool vp_url_parse(const char *base, vp_url_t *out);

/* The same check, without parsing into a struct (for small stacks). */
bool vp_url_valid(const char *base);

/* Writes "<scheme>://<host>[:port]<path><suffix>" (suffix starts with '/').
 * False if it doesn't fit. */
bool vp_url_join(const vp_url_t *u, const char *suffix, char *out, size_t cap);

/*
 * Resolves message_done.audio_url against the turn URL (base path + "/turn"),
 * as the protocol says (RFC 3986, relative reference): "audio/<id>.mp3"
 * becomes <base>/audio/<id>.mp3, "/audio/<id>.mp3" becomes
 * <origin>/audio/<id>.mp3. The node sends its bearer token to this URL, so
 * anything that could point elsewhere is refused: absolute URLs (with a
 * scheme), network-path references ("//host/..."), "." / ".." segments,
 * backslashes, '%', query/fragment, whitespace and control characters.
 */
bool vp_resolve_audio_url(const vp_url_t *base, const char *ref, char *out, size_t cap);

/* ---- Request ---- */

typedef struct {
    const char *name;
    const char *value;
} vp_header_t;

#define VP_TURN_HEADERS 5

/*
 * The headers of POST <base>/turn, in order: Authorization, X-Node-Id,
 * X-Vesper-Node-Protocol, Content-Type, Accept. The Authorization value is
 * written into auth_buf (VP_AUTH_MAX). False (and nothing usable) if the
 * token or node id is empty, too long, or has characters that could break
 * the header (anything outside printable ASCII without spaces for the token;
 * outside [A-Za-z0-9._:-] for the node id).
 */
bool vp_turn_headers(const char *token, const char *node_id, char auth_buf[VP_AUTH_MAX],
                     vp_header_t out[VP_TURN_HEADERS]);

bool vp_valid_token(const char *token);
bool vp_valid_node_id(const char *node_id);

/* The 44-byte streaming WAV header (RIFF and data sizes 0xFFFFFFFF), mono
 * PCM16 at `rate`: the same bytes as the stock muse_hatch_wav_header()
 * (muse_chat_text.c), which the firmware uses; this copy is for the host
 * tests and the live-turn harness, which don't link the SDK. */
void vp_wav_header(uint8_t h[VP_WAV_HEADER], uint32_t rate);

/* ---- Responses before the stream ---- */

typedef enum {
    VP_HTTP_STREAM,      /* 200 text/event-stream: read the SSE reply */
    VP_HTTP_RETRY,       /* 503 busy: retry once after *retry_ms */
    VP_HTTP_FAIL,        /* show caption, end the turn as an error */
} vp_http_verdict_t;

/*
 * What to do with the turn's HTTP status. content_type: the response's
 * Content-Type (may be NULL); error_code: the "error" field of a JSON error
 * body (may be NULL); retry_after: the Retry-After header (may be NULL);
 * attempt: 0 for the first POST, 1 for the retry. caption gets a short,
 * fixed, upper-case message for the screen (cap >= 32 recommended).
 */
vp_http_verdict_t vp_http_verdict(int status, const char *content_type, const char *error_code,
                                  const char *retry_after, int attempt, int *retry_ms,
                                  char *caption, size_t cap);

/* The screen caption for an SSE `error` event: the server's message if it
 * has one, else a fixed one per code (unknown codes get a generic one). */
void vp_error_caption(const char *code, const char *message, char *out, size_t cap);

/* ---- Minimal JSON (flat objects, as every v1 event's data is) ---- */

typedef enum {
    VP_JSON_MISSING,
    VP_JSON_NULL,
    VP_JSON_STRING,
    VP_JSON_OTHER,      /* present, but a number/bool/object/array */
    VP_JSON_BAD,        /* not a well-formed object */
} vp_json_kind_t;

/*
 * Finds the top-level member `key` of the JSON object json[0..len) and, if it
 * is a string, decodes it (escapes, \uXXXX with surrogate pairs; invalid ones
 * become U+FFFD) into out, cut UTF-8-safely to cap. Nested members never
 * match. Nesting deeper than 16 levels counts as malformed.
 */
vp_json_kind_t vp_json_string(const char *json, size_t len, const char *key, char *out, size_t cap);

/* The top-level boolean member `key`; *found false if it's missing or not a bool. */
bool vp_json_bool(const char *json, size_t len, const char *key, bool *found);

/* The top-level member `key` if it is a plain non-negative integer (digits only: no sign,
 * fraction or exponent) no larger than UINT32_MAX. False otherwise (*out is then 0). */
bool vp_json_uint(const char *json, size_t len, const char *key, uint32_t *out);

/*
 * Arrays (task 18: GET /announcements). vp_json_object_ok: json[0..len) is one object and
 * nothing but whitespace follows it (truncated or trailing junk is refused). vp_json_array:
 * the top-level member `key` is an array; its elements are then walked with vp_json_next,
 * which gives each element's span (an object element is then read with vp_json_string). It
 * returns false at the end of the array, or when the array is malformed (it->bad).
 */
typedef struct {
    const char *p, *end;
    bool first, done, bad;
} vp_json_iter_t;

bool vp_json_object_ok(const char *json, size_t len);
bool vp_json_array(const char *json, size_t len, const char *key, vp_json_iter_t *it);
bool vp_json_next(vp_json_iter_t *it, const char **elem, size_t *elem_len);

/* ---- SSE framing ---- */

typedef void (*vp_sse_event_cb)(void *ctx, const char *event, const char *data, size_t len);

typedef struct {
    char line[VP_SSE_LINE_MAX];
    size_t line_len;
    bool line_skip;          /* a comment, or a line that outgrew the buffer */
    bool line_oversized;
    bool after_cr;           /* the last byte was CR: a following LF ends nothing */
    char event[VP_SSE_EVENT_MAX];
    char data[VP_SSE_DATA_MAX];
    size_t data_len;
    bool have_data;
    bool broken;             /* this event lost a line or data: drop it */
    unsigned dropped;        /* events dropped as broken (diagnostics) */
    vp_sse_event_cb cb;
    void *ctx;
} vp_sse_t;

void vp_sse_init(vp_sse_t *s, vp_sse_event_cb cb, void *ctx);
/* Feeds any number of bytes, split anywhere (even inside a CRLF or a UTF-8
 * character). Calls cb for each complete event, in order. */
void vp_sse_feed(vp_sse_t *s, const char *buf, size_t n);
/* End of stream: an unterminated event is discarded, as the SSE spec says. */
void vp_sse_finish(vp_sse_t *s);

/* ---- A turn's events -> firmware actions ---- */

typedef struct {
    void (*transcript)(void *ctx, const char *text);
    void (*message_start)(void *ctx, const char *id);
    void (*text_delta)(void *ctx, const char *id, const char *text);
    /* audio_url: the resolved absolute URL, or NULL (no audio / refused) */
    void (*message_done)(void *ctx, const char *id, const char *audio_url);
    void (*error)(void *ctx, const char *code, const char *caption);
    void (*done)(void *ctx, bool ok);
} vp_turn_cbs_t;

typedef struct {
    vp_sse_t sse;
    const vp_turn_cbs_t *cbs;
    void *ctx;
    const vp_url_t *base;
    bool done;               /* `done` arrived: nothing after it counts */
    bool errored;
    bool in_message;
    char msg_id[VP_ID_MAX];
    unsigned messages;
    unsigned unknown_events;
    unsigned bad_events;     /* known events with malformed data */
    char scratch[VP_TEXT_MAX];
    char scratch2[VP_TEXT_MAX];
} vp_turn_t;

void vp_turn_init(vp_turn_t *t, const vp_url_t *base, const vp_turn_cbs_t *cbs, void *ctx);
void vp_turn_feed(vp_turn_t *t, const char *buf, size_t n);
/* The stream ended. Returns true if it ended properly (a `done` event). */
bool vp_turn_finish(vp_turn_t *t);

#ifdef __cplusplus
}
#endif
