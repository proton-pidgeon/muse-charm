/*
 * Host tests for firmware/hatch/vesper_proto.c (the protocol core of the
 * Vesper muse_hatch_* backend). Run: make -C firmware test
 */
#include "vesper_proto.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail, s_checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        s_checks++;                                                              \
        if (!(cond)) {                                                           \
            s_fail++;                                                            \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

#define CHECK_STR(a, b)                                                                     \
    do {                                                                                    \
        s_checks++;                                                                         \
        const char *a_ = (a), *b_ = (b);                                                    \
        if (!a_ || !b_ || strcmp(a_, b_) != 0) {                                            \
            s_fail++;                                                                       \
            fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, a_ ? a_ : "(null)", \
                    b_ ? b_ : "(null)");                                                    \
        }                                                                                   \
    } while (0)

/* ---- a recording of a turn's callbacks ---- */

typedef struct {
    char log[16384];
    char caption[VP_TEXT_MAX];   /* text deltas concatenated, as the firmware does */
    char audio[VP_URL_MAX];
    char err[VP_TEXT_MAX];
    int done;                    /* -1 none, 0 false, 1 true */
    int starts, deltas, dones, errors, transcripts;
} rec_t;

static void logf_(rec_t *r, const char *fmt, ...)
{
    size_t n = strlen(r->log);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->log + n, sizeof(r->log) - n, fmt, ap);
    va_end(ap);
}

static void on_transcript(void *ctx, const char *text)
{
    rec_t *r = ctx;
    r->transcripts++;
    logf_(r, "T(%s)", text);
}

static void on_start(void *ctx, const char *id)
{
    rec_t *r = ctx;
    r->starts++;
    r->caption[0] = '\0';
    logf_(r, "S(%s)", id);
}

static void on_delta(void *ctx, const char *id, const char *text)
{
    rec_t *r = ctx;
    r->deltas++;
    vp_utf8_append(r->caption, sizeof(r->caption), text);
    logf_(r, "D(%s,%s)", id, text);
}

static void on_mdone(void *ctx, const char *id, const char *url)
{
    rec_t *r = ctx;
    r->dones++;
    snprintf(r->audio, sizeof(r->audio), "%s", url ? url : "(null)");
    logf_(r, "M(%s,%s)", id, url ? url : "null");
}

static void on_error(void *ctx, const char *code, const char *caption)
{
    rec_t *r = ctx;
    r->errors++;
    snprintf(r->err, sizeof(r->err), "%s", caption);
    logf_(r, "E(%s,%s)", code, caption);
}

static void on_done(void *ctx, bool ok)
{
    rec_t *r = ctx;
    r->done = ok;
    logf_(r, "F(%d)", ok);
}

static const vp_turn_cbs_t CBS = { on_transcript, on_start, on_delta, on_mdone, on_error, on_done };

static vp_turn_t s_turn;   /* ~16 KB: keep it off the stack */

static void run_turn(const vp_url_t *base, const char *stream, size_t len, size_t chunk, rec_t *r, bool *finished)
{
    memset(r, 0, sizeof(*r));
    r->done = -1;
    vp_turn_init(&s_turn, base, &CBS, r);
    for (size_t off = 0; off < len;) {
        size_t n = len - off < chunk ? len - off : chunk;
        vp_turn_feed(&s_turn, stream + off, n);
        off += n;
    }
    *finished = vp_turn_finish(&s_turn);
}

/* ---- the canonical v1 stream, with the 2 KB priming preamble ---- */

static char *make_stream(const char *body, size_t *len)
{
    size_t cap = 4096 + strlen(body);
    char *s = malloc(cap);
    size_t n = 0;
    s[n++] = ':';
    s[n++] = ' ';
    for (int i = 0; i < 2048; i++) {
        s[n++] = ' ';
    }
    s[n++] = '\n';
    s[n++] = '\n';
    memcpy(s + n, body, strlen(body));
    n += strlen(body);
    *len = n;
    return s;
}

static const char NORMAL[] =
    "event: transcript\ndata: {\"text\": \"what is two plus two\"}\n\n"
    ": ping\n\n"
    "event: message_start\ndata: {\"id\": \"m1\"}\n\n"
    "event: text_delta\ndata: {\"id\": \"m1\", \"text\": \"Two plus two \"}\n\n"
    "event: text_delta\ndata: {\"id\":\"m1\",\"text\":\"is four.\"}\n\n"
    "event: message_done\ndata: {\"id\": \"m1\", \"audio_url\": \"audio/AbC_-123.mp3\", \"audio_bytes\": 4096}\n\n"
    "event: timing\ndata: {\"upload_ms\": 10, \"stt_ms\": 400, \"ask_ms\": 600, \"tts_ms\": 300, \"total_ms\": 1300, \"stt_provider\": \"elevenlabs\"}\n\n"
    "event: done\ndata: {\"ok\": true}\n\n";

static void test_turn_normal(void)
{
    vp_url_t base;
    CHECK(vp_url_parse("https://peggy.fly.dev/vesper-node", &base));
    size_t len;
    char *stream = make_stream(NORMAL, &len);
    const char *want =
        "T(what is two plus two)S(m1)D(m1,Two plus two )D(m1,is four.)"
        "M(m1,https://peggy.fly.dev/vesper-node/audio/AbC_-123.mp3)F(1)";
    /* every chunk size, including one byte at a time (splits inside the
     * preamble, inside "\r\n"-free lines, inside JSON and UTF-8) */
    size_t sizes[] = { 1, 2, 3, 7, 13, 64, 511, 2049, 100000 };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        rec_t r;
        bool fin;
        run_turn(&base, stream, len, sizes[i], &r, &fin);
        CHECK(fin);
        CHECK_STR(r.log, want);
        CHECK_STR(r.caption, "Two plus two is four.");
        CHECK(s_turn.sse.dropped == 0);
    }
    free(stream);
}

static void test_turn_crlf_and_cr(void)
{
    vp_url_t base;
    CHECK(vp_url_parse("http://[::1]:8796", &base));
    char crlf[sizeof(NORMAL) * 2], cr[sizeof(NORMAL)];
    size_t a = 0, b = 0;
    for (const char *p = NORMAL; *p; p++) {
        if (*p == '\n') {
            crlf[a++] = '\r';
            crlf[a++] = '\n';
            cr[b++] = '\r';
        } else {
            crlf[a++] = *p;
            cr[b++] = *p;
        }
    }
    const char *want =
        "T(what is two plus two)S(m1)D(m1,Two plus two )D(m1,is four.)"
        "M(m1,http://[::1]:8796/audio/AbC_-123.mp3)F(1)";
    for (size_t chunk = 1; chunk <= 5; chunk++) {
        rec_t r;
        bool fin;
        run_turn(&base, crlf, a, chunk, &r, &fin);
        CHECK(fin);
        CHECK_STR(r.log, want);
        run_turn(&base, cr, b, chunk, &r, &fin);
        CHECK(fin);
        CHECK_STR(r.log, want);
    }
}

static void test_turn_error(void)
{
    vp_url_t base;
    CHECK(vp_url_parse("http://[::1]:8796", &base));
    const char *s =
        ": " "xxxxxxxx\n\n"
        "event: transcript\ndata: {\"text\": \"\"}\n\n"
        "event: error\ndata: {\"code\": \"empty_transcript\", \"message\": \"I didn't catch that.\"}\n\n"
        "event: timing\ndata: {\"total_ms\": 5}\n\n"
        "event: done\ndata: {\"ok\": false}\n\n";
    rec_t r;
    bool fin;
    run_turn(&base, s, strlen(s), 5, &r, &fin);
    CHECK(fin);
    CHECK_STR(r.log, "T()E(empty_transcript,I didn't catch that.)F(0)");
    CHECK(r.errors == 1 && r.starts == 0);

    /* an error without a message: the per-code caption; unknown codes are generic */
    s = "event: error\ndata: {\"code\": \"stt_failed\", \"message\": \"\"}\n\n"
        "event: done\ndata: {\"ok\": false}\n\n";
    run_turn(&base, s, strlen(s), 1, &r, &fin);
    CHECK_STR(r.err, "COULDN'T TRANSCRIBE");
    s = "event: error\ndata: {\"code\": \"brand_new_code\"}\n\nevent: done\ndata: {\"ok\":false}\n\n";
    run_turn(&base, s, strlen(s), 1, &r, &fin);
    CHECK_STR(r.err, "SOMETHING WENT WRONG");
    CHECK(r.done == 0);

    /* ok:true after an error still ends the turn as failed */
    s = "event: error\ndata: {\"code\": \"internal\", \"message\": \"x\"}\n\nevent: done\ndata: {\"ok\":true}\n\n";
    run_turn(&base, s, strlen(s), 3, &r, &fin);
    CHECK(r.done == 0);
}

static void test_turn_robustness(void)
{
    vp_url_t base;
    CHECK(vp_url_parse("https://peggy.fly.dev/vesper-node/", &base));
    rec_t r;
    bool fin;

    /* no done: the stream was cut */
    const char *s = "event: message_start\ndata: {\"id\":\"m1\"}\n\nevent: text_delta\ndata: {\"id\":\"m1\",\"text\":\"hi\"}\n\n";
    run_turn(&base, s, strlen(s), 4, &r, &fin);
    CHECK(!fin);
    CHECK(r.done == -1);
    CHECK_STR(r.caption, "hi");

    /* an unterminated last event is discarded (SSE spec) */
    s = "event: done\ndata: {\"ok\": true}\n";
    run_turn(&base, s, strlen(s), 4, &r, &fin);
    CHECK(!fin);

    /* unknown events and fields are ignored; events after done are ignored */
    s = "event: shiny_new\ndata: {\"x\": 1}\n\n"
        "id: 7\nretry: 100\nevent: message_start\ndata: {\"id\":\"m1\",\"extra\":{\"text\":\"nested\"}}\n\n"
        "event: text_delta\ndata: {\"id\":\"m1\",\"meta\":{\"text\":\"WRONG\"},\"text\":\"right\"}\n\n"
        "event: message_done\ndata: {\"id\":\"m1\",\"audio_url\":null,\"audio_bytes\":null}\n\n"
        "event: done\ndata: {\"ok\":true}\n\n"
        "event: text_delta\ndata: {\"id\":\"m1\",\"text\":\"late\"}\n\n";
    run_turn(&base, s, strlen(s), 2, &r, &fin);
    CHECK(fin);
    CHECK_STR(r.log, "S(m1)D(m1,right)M(m1,null)F(1)");
    CHECK(s_turn.unknown_events == 1);

    /* a delta with no message_start opens the message itself; multiple
     * messages each get their own start/done */
    s = "event: text_delta\ndata: {\"id\":\"a\",\"text\":\"one\"}\n\n"
        "event: message_done\ndata: {\"id\":\"a\",\"audio_url\":\"audio/x.mp3\"}\n\n"
        "event: message_start\ndata: {\"id\":\"b\"}\n\n"
        "event: text_delta\ndata: {\"id\":\"b\",\"text\":\"two\"}\n\n"
        "event: message_done\ndata: {\"id\":\"b\"}\n\n"
        "event: done\ndata: {\"ok\":true}\n\n";
    run_turn(&base, s, strlen(s), 9, &r, &fin);
    CHECK_STR(r.log, "S(a)D(a,one)M(a,https://peggy.fly.dev/vesper-node/audio/x.mp3)S(b)D(b,two)M(b,null)F(1)");

    /* a hostile audio_url is refused (the node would send its token there) */
    s = "event: message_start\ndata: {\"id\":\"m1\"}\n\n"
        "event: message_done\ndata: {\"id\":\"m1\",\"audio_url\":\"https://evil.example/a.mp3\"}\n\n"
        "event: done\ndata: {\"ok\":true}\n\n";
    run_turn(&base, s, strlen(s), 64, &r, &fin);
    CHECK_STR(r.log, "S(m1)M(m1,null)F(1)");

    /* malformed JSON is counted and skipped; the stream carries on */
    s = "event: text_delta\ndata: {\"id\":\"m1\",\"text\":\"unterminated}\n\n"
        "event: transcript\ndata: not json\n\n"
        "event: done\ndata: {\"ok\":true}\n\n";
    run_turn(&base, s, strlen(s), 1, &r, &fin);
    CHECK(fin);
    CHECK(s_turn.bad_events == 2);
    CHECK(r.deltas == 0 && r.transcripts == 0);

    /* multi-line data joins with \n (and so isn't valid JSON for a delta) */
    s = "event: transcript\ndata: {\"text\":\ndata: \"split\"}\n\nevent: done\ndata: {\"ok\":true}\n\n";
    run_turn(&base, s, strlen(s), 1, &r, &fin);
    CHECK_STR(r.log, "T(split)F(1)");

    /* an event with no data isn't dispatched */
    s = "event: done\n\nevent: done\ndata: {\"ok\":true}\n\n";
    run_turn(&base, s, strlen(s), 1, &r, &fin);
    CHECK_STR(r.log, "F(1)");
}

static void test_sse_oversized(void)
{
    vp_url_t base;
    CHECK(vp_url_parse("http://[::1]:8796", &base));
    /* a 10 KB data line: that event is dropped, the next one survives */
    size_t big = 10000;
    char *s = malloc(big + 512);
    size_t n = 0;
    n += (size_t)snprintf(s + n, big + 512 - n, "event: text_delta\ndata: {\"id\":\"m1\",\"text\":\"");
    for (size_t i = 0; i < big; i++) {
        s[n++] = 'a';
    }
    n += (size_t)snprintf(s + n, big + 512 - n, "\"}\n\nevent: message_start\ndata: {\"id\":\"m2\"}\n\nevent: done\ndata: {\"ok\":true}\n\n");
    rec_t r;
    bool fin;
    run_turn(&base, s, n, 333, &r, &fin);
    CHECK(fin);
    CHECK_STR(r.log, "S(m2)F(1)");
    CHECK(s_turn.sse.dropped == 1);

    /* a huge comment (bigger than every buffer) is fine */
    n = 0;
    s[n++] = ':';
    for (size_t i = 0; i < big; i++) {
        s[n++] = 'p';
    }
    n += (size_t)snprintf(s + n, big + 512 - n, "\nevent: done\ndata: {\"ok\":true}\n\n");
    run_turn(&base, s, n, 77, &r, &fin);
    CHECK(fin);
    CHECK(s_turn.sse.dropped == 0);

    /* data lines that together outgrow the event's buffer: dropped */
    n = 0;
    n += (size_t)snprintf(s + n, big + 512 - n, "event: transcript\n");
    for (int k = 0; k < 6; k++) {
        n += (size_t)snprintf(s + n, big + 512 - n, "data: ");
        for (int i = 0; i < 1000; i++) {
            s[n++] = 'b';
        }
        s[n++] = '\n';
    }
    n += (size_t)snprintf(s + n, big + 512 - n, "\nevent: done\ndata: {\"ok\":true}\n\n");
    run_turn(&base, s, n, 1, &r, &fin);
    CHECK_STR(r.log, "F(1)");
    CHECK(s_turn.sse.dropped == 1);
    free(s);
}

/* ---- JSON ---- */

static void test_json(void)
{
    char out[64];
    const char *j = "{\"text\": \"caf\\u00e9 \\\"q\\\" \\\\ \\/ \\n\\t end\", \"n\": 3, \"b\": true, \"z\": null}";
    CHECK(vp_json_string(j, strlen(j), "text", out, sizeof(out)) == VP_JSON_STRING);
    CHECK_STR(out, "caf\xC3\xA9 \"q\" \\ / \n\t end");
    CHECK(vp_json_string(j, strlen(j), "n", out, sizeof(out)) == VP_JSON_OTHER);
    CHECK(vp_json_string(j, strlen(j), "z", out, sizeof(out)) == VP_JSON_NULL);
    CHECK(vp_json_string(j, strlen(j), "missing", out, sizeof(out)) == VP_JSON_MISSING);
    bool found;
    CHECK(vp_json_bool(j, strlen(j), "b", &found) && found);
    CHECK(!vp_json_bool(j, strlen(j), "n", &found) && !found);
    CHECK(!vp_json_bool("{\"ok\":false}", 12, "ok", &found) && found);

    /* surrogate pairs, lone surrogates, raw UTF-8 */
    j = "{\"t\":\"\\ud83e\\udd89 owl \\ud800x \\udc00 \xE2\x98\x85\"}";
    CHECK(vp_json_string(j, strlen(j), "t", out, sizeof(out)) == VP_JSON_STRING);
    CHECK_STR(out, "\xF0\x9F\xA6\x89 owl \xEF\xBF\xBDx \xEF\xBF\xBD \xE2\x98\x85");
    /* \u0000 never becomes a NUL */
    j = "{\"t\":\"a\\u0000b\"}";
    CHECK(vp_json_string(j, strlen(j), "t", out, sizeof(out)) == VP_JSON_STRING);
    CHECK_STR(out, "a\xEF\xBF\xBD" "b");

    /* nested members never match; arrays and objects are skipped */
    j = "{\"a\":{\"text\":\"no\",\"x\":[1,{\"text\":\"no\"},\"]}\"]},\"text\":\"yes\"}";
    CHECK(vp_json_string(j, strlen(j), "text", out, sizeof(out)) == VP_JSON_STRING);
    CHECK_STR(out, "yes");

    /* malformed */
    const char *bad[] = { "", "[]", "{", "{\"a\"}", "{\"a\":}", "{\"a\":\"x}", "{\"a\":\"\x01\"}",
                          "{\"a\":1 \"b\":2}", "{\"a\":\"\\q\"}", "{\"a\":\"\\u12\"}" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(vp_json_string(bad[i], strlen(bad[i]), "b", out, sizeof(out)) == VP_JSON_BAD);
    }
    /* nesting deeper than 16 is refused, not recursed */
    char deep[200];
    size_t n = (size_t)snprintf(deep, sizeof(deep), "{\"a\":");
    for (int i = 0; i < 40; i++) {
        deep[n++] = '[';
    }
    for (int i = 0; i < 40; i++) {
        deep[n++] = ']';
    }
    n += (size_t)snprintf(deep + n, sizeof(deep) - n, ",\"b\":\"x\"}");
    CHECK(vp_json_string(deep, n, "b", out, sizeof(out)) == VP_JSON_BAD);

    /* the decoded string is cut on a character boundary, as a prefix */
    char small[6];
    j = "{\"t\":\"ab\\u00e9\\u00e9z\"}";   /* a b é(2) é(2) z */
    CHECK(vp_json_string(j, strlen(j), "t", small, sizeof(small)) == VP_JSON_STRING);
    CHECK_STR(small, "ab\xC3\xA9");   /* 4 bytes; the next é would need 6 */
    j = "{\"t\":\"\\ud83e\\udd89\\ud83e\\udd89\"}";
    CHECK(vp_json_string(j, strlen(j), "t", small, sizeof(small)) == VP_JSON_STRING);
    CHECK_STR(small, "\xF0\x9F\xA6\x89");
}

/* ---- UTF-8 ---- */

static void test_utf8(void)
{
    char out[8];
    /* "h é l l o" with é split by the cap */
    CHECK(vp_utf8_copy(out, 3, "h\xC3\xA9llo", 6) == 1);
    CHECK_STR(out, "h");
    CHECK(vp_utf8_copy(out, 4, "h\xC3\xA9llo", 6) == 3);
    CHECK_STR(out, "h\xC3\xA9");
    /* a 4-byte character never half-copied */
    CHECK(vp_utf8_copy(out, 4, "\xF0\x9F\xA6\x89", 4) == 0);
    CHECK_STR(out, "");
    CHECK(vp_utf8_copy(out, 5, "\xF0\x9F\xA6\x89", 4) == 4);
    /* a character cut off by the input's end is left out */
    CHECK(vp_utf8_copy(out, 8, "ab\xE2\x98", 4) == 2);
    CHECK_STR(out, "ab");
    /* invalid bytes pass through one at a time (they can't overflow) */
    CHECK(vp_utf8_copy(out, 8, "\xFF\x80z", 3) == 3);
    /* append */
    char buf[8] = "ab";
    CHECK(vp_utf8_append(buf, sizeof(buf), "cd") == 2);
    CHECK_STR(buf, "abcd");
    CHECK(vp_utf8_append(buf, sizeof(buf), "\xE2\x98\x85") == 3);   /* exactly fills 7 */
    CHECK_STR(buf, "abcd\xE2\x98\x85");
    CHECK(vp_utf8_append(buf, sizeof(buf), "z") == 0);
    CHECK(vp_utf8_copy(out, 0, "x", 1) == 0);
}

/* ---- URLs ---- */

static void test_urls(void)
{
    vp_url_t u;
    char out[VP_URL_MAX];
    CHECK(vp_url_parse("https://peggy.fly.dev/vesper-node", &u));
    CHECK(u.https && u.port == 443 && !strcmp(u.host, "peggy.fly.dev") && !strcmp(u.path, "/vesper-node"));
    CHECK(vp_url_join(&u, "/turn", out, sizeof(out)));
    CHECK_STR(out, "https://peggy.fly.dev/vesper-node/turn");
    CHECK(vp_url_parse("HTTPS://peggy.fly.dev/vesper-node///", &u));
    CHECK(vp_url_join(&u, "/turn", out, sizeof(out)));
    CHECK_STR(out, "https://peggy.fly.dev/vesper-node/turn");
    CHECK(vp_url_parse("http://[::1]:8796", &u));
    CHECK(!u.https && u.ipv6 && u.port == 8796 && !strcmp(u.host, "::1") && !strcmp(u.path, ""));
    CHECK(vp_url_join(&u, "/turn", out, sizeof(out)));
    CHECK_STR(out, "http://[::1]:8796/turn");
    CHECK(vp_url_parse("http://[fdaa:3e:60bd:a7b:9016:9c37:ac65:d902]:8796/", &u));
    CHECK(vp_url_join(&u, "/turn", out, sizeof(out)));
    CHECK_STR(out, "http://[fdaa:3e:60bd:a7b:9016:9c37:ac65:d902]:8796/turn");
    CHECK(vp_url_parse("http://192.168.1.20:8796", &u));
    CHECK(vp_url_parse("https://example.com:443/x", &u));
    CHECK(vp_url_join(&u, "/turn", out, sizeof(out)));
    CHECK_STR(out, "https://example.com/x/turn");

    const char *bad[] = { NULL, "", "hatch.metaaivm.com", "ftp://x", "http://", "http://user@host/",
                          "http://host/?q=1", "http://host/#f", "http://ho st/", "http://host:0/",
                          "http://host:70000/", "http://host:/", "http://[::1/", "http://[zz]/",
                          "http://host/a b", "http://host\r\n/", "http://host/a\\b" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(!vp_url_parse(bad[i], &u));
    }
    char longurl[400];
    snprintf(longurl, sizeof(longurl), "https://h/%0300d", 0);
    CHECK(!vp_url_parse(longurl, &u));

    /* audio_url resolution */
    CHECK(vp_url_parse("https://peggy.fly.dev/vesper-node", &u));
    CHECK(vp_resolve_audio_url(&u, "audio/abcDEF123_-xyz.mp3", out, sizeof(out)));
    CHECK_STR(out, "https://peggy.fly.dev/vesper-node/audio/abcDEF123_-xyz.mp3");
    CHECK(vp_resolve_audio_url(&u, "/audio/x.mp3", out, sizeof(out)));
    CHECK_STR(out, "https://peggy.fly.dev/audio/x.mp3");
    CHECK(vp_url_parse("http://[::1]:8796", &u));
    CHECK(vp_resolve_audio_url(&u, "audio/x.mp3", out, sizeof(out)));
    CHECK_STR(out, "http://[::1]:8796/audio/x.mp3");
    const char *refs[] = { NULL, "", "http://evil/a.mp3", "https:audio/a.mp3", "//evil/a.mp3", "../a.mp3",
                           "audio/../../x", "./a.mp3", "audio//a.mp3", "audio/a.mp3?x=1", "audio/a.mp3#f",
                           "audio/%2e%2e/a", "audio\\a.mp3", "audio/a b.mp3", "audio/a\n.mp3", "/" };
    for (size_t i = 0; i < sizeof(refs) / sizeof(refs[0]); i++) {
        CHECK(!vp_resolve_audio_url(&u, refs[i], out, sizeof(out)));
    }
    char longref[300];
    memset(longref, 'a', sizeof(longref) - 1);
    longref[sizeof(longref) - 1] = '\0';
    CHECK(!vp_resolve_audio_url(&u, longref, out, sizeof(out)));
    /* too small an output fails rather than truncating */
    char tiny[20];
    CHECK(!vp_resolve_audio_url(&u, "audio/x.mp3", tiny, sizeof(tiny)));
}

/* ---- request ---- */

static void test_headers(void)
{
    char auth[VP_AUTH_MAX];
    vp_header_t h[VP_TURN_HEADERS];
    const char *tok = "tok_0123456789abcdefghijklmnopqrstuvwxyz-_ABC";   /* a fixture, not a real token */
    CHECK(vp_turn_headers(tok, "homelink-aabbccddeeff", auth, h));
    CHECK_STR(h[0].name, "Authorization");
    CHECK(!strncmp(h[0].value, "Bearer ", 7) && !strcmp(h[0].value + 7, tok));
    CHECK_STR(h[1].name, "X-Node-Id");
    CHECK_STR(h[1].value, "homelink-aabbccddeeff");
    CHECK_STR(h[2].name, "X-Vesper-Node-Protocol");
    CHECK_STR(h[2].value, "1");
    CHECK_STR(h[3].name, "Content-Type");
    CHECK_STR(h[3].value, "audio/wav");
    CHECK_STR(h[4].name, "Accept");
    CHECK_STR(h[4].value, "text/event-stream");

    /* header injection and junk are refused */
    CHECK(!vp_turn_headers("", "homelink-1", auth, h));
    CHECK(!vp_turn_headers(NULL, "homelink-1", auth, h));
    CHECK(!vp_turn_headers("abc def", "homelink-1", auth, h));
    CHECK(!vp_turn_headers("abc\r\nX-Evil: 1", "homelink-1", auth, h));
    CHECK(!vp_turn_headers("abc\x7f", "homelink-1", auth, h));
    CHECK(!vp_turn_headers("abc", "", auth, h));
    CHECK(!vp_turn_headers("abc", "home link", auth, h));
    CHECK(!vp_turn_headers("abc", "a/b", auth, h));
    CHECK(!vp_turn_headers("abc", "homelink-1\r\n", auth, h));
    CHECK(auth[0] == '\0');
    char id[200];
    memset(id, 'a', 128);
    id[128] = '\0';
    CHECK(vp_valid_node_id(id));
    id[128] = 'a';
    id[129] = '\0';
    CHECK(!vp_valid_node_id(id));
    CHECK(vp_valid_node_id("a.b_c:d-E9"));
    static char longtok[VP_TOKEN_MAX + 2];
    memset(longtok, 'x', VP_TOKEN_MAX);
    CHECK(vp_valid_token(longtok));
    longtok[VP_TOKEN_MAX] = 'x';
    CHECK(!vp_valid_token(longtok));
}

static void test_wav(void)
{
    static const uint8_t want[44] = {
        'R', 'I', 'F', 'F', 0xFF, 0xFF, 0xFF, 0xFF, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
        16, 0, 0, 0, 1, 0, 1, 0, 0x80, 0x3E, 0, 0, 0x00, 0x7D, 0, 0,
        2, 0, 16, 0, 'd', 'a', 't', 'a', 0xFF, 0xFF, 0xFF, 0xFF,
    };
    uint8_t h[VP_WAV_HEADER];
    vp_wav_header(h, 16000);
    CHECK(!memcmp(h, want, sizeof(want)));
}

static void test_http(void)
{
    char cap[40];
    int ms;
    CHECK(vp_http_verdict(200, "text/event-stream; charset=utf-8", NULL, NULL, 0, &ms, cap, sizeof(cap)) == VP_HTTP_STREAM);
    CHECK(vp_http_verdict(200, "Text/Event-Stream", NULL, NULL, 0, &ms, cap, sizeof(cap)) == VP_HTTP_STREAM);
    CHECK(vp_http_verdict(200, "application/json", NULL, NULL, 0, &ms, cap, sizeof(cap)) == VP_HTTP_FAIL);
    CHECK(vp_http_verdict(200, NULL, NULL, NULL, 0, &ms, cap, sizeof(cap)) == VP_HTTP_FAIL);
    CHECK(vp_http_verdict(503, "application/json", "busy", "2", 0, &ms, cap, sizeof(cap)) == VP_HTTP_RETRY && ms == 2000);
    CHECK(vp_http_verdict(503, NULL, "busy", "60", 0, &ms, cap, sizeof(cap)) == VP_HTTP_RETRY && ms == 5000);
    CHECK(vp_http_verdict(503, NULL, "busy", "0", 0, &ms, cap, sizeof(cap)) == VP_HTTP_RETRY && ms == 1000);
    CHECK(vp_http_verdict(503, NULL, "busy", "Wed, 21 Oct 2026 07:28:00 GMT", 0, &ms, cap, sizeof(cap)) == VP_HTTP_RETRY && ms == 2000);
    CHECK(vp_http_verdict(503, NULL, "busy", NULL, 0, &ms, cap, sizeof(cap)) == VP_HTTP_RETRY && ms == 2000);
    CHECK(vp_http_verdict(503, NULL, "busy", "2", 1, &ms, cap, sizeof(cap)) == VP_HTTP_FAIL);
    CHECK_STR(cap, "VESPER IS BUSY");
    struct { int st; const char *code; const char *cap; } cases[] = {
        { 401, "unauthorized", "TOKEN REFUSED" },
        { 400, "bad_node_id", "BAD DEVICE ID" },
        { 400, "unsupported_protocol", "UPDATE THE FIRMWARE" },
        { 400, NULL, "REQUEST REFUSED" },
        { 413, "too_large", "NOTE TOO LONG" },
        { 415, "unsupported_media_type", "BAD AUDIO TYPE" },
        { 422, "bad_audio", "COULDN'T READ THE AUDIO" },
        { 408, "upload_timeout", "UPLOAD TOO SLOW" },
        { 404, "not_found", "CHECK THE SERVER URL" },
        { 405, NULL, "CHECK THE SERVER URL" },
        { 500, NULL, "VESPER SERVER ERROR" },
        { 0, NULL, "CAN'T REACH VESPER" },
        { -1, NULL, "CAN'T REACH VESPER" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        CHECK(vp_http_verdict(cases[i].st, "application/json", cases[i].code, NULL, 0, &ms, cap, sizeof(cap)) == VP_HTTP_FAIL);
        CHECK_STR(cap, cases[i].cap);
    }
    /* never retries anything but 503 */
    CHECK(vp_http_verdict(502, NULL, NULL, "1", 0, &ms, cap, sizeof(cap)) == VP_HTTP_FAIL && ms == 0);

    /* error captions are cut UTF-8-safely */
    char small[8];
    vp_error_caption("ask_failed", "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", small, sizeof(small));
    CHECK_STR(small, "\xC3\xA9\xC3\xA9\xC3\xA9");
    vp_error_caption("ask_failed", NULL, cap, sizeof(cap));
    CHECK_STR(cap, "VESPER DIDN'T ANSWER");
}

int main(void)
{
    test_utf8();
    test_json();
    test_urls();
    test_headers();
    test_wav();
    test_http();
    test_turn_normal();
    test_turn_crlf_and_cr();
    test_turn_error();
    test_turn_robustness();
    test_sse_oversized();
    printf("vesper_proto: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
