/*
 * One real push-to-talk turn against a running node backend, through the
 * firmware's own protocol code (vesper_proto.c): the same headers, the same
 * streaming WAV header + 16 kHz PCM16 body (sent chunked, as the device
 * streams it while the button is held), the same SSE parser, captions and
 * audio_url resolution. libcurl stands in for esp_http_client.
 *
 *   live_turn <base-url> <note.wav>      token from $VESPER_NODE_TOKEN
 *
 * The token is never printed. Audio ids are shown redacted. Use
 * hatch/test/live_turn.sh, which makes the note and reads the token.
 */
#include "vesper_proto.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define NODE_ID "homelink-hosttest"   /* not a real board's id */

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- the note: the firmware's streaming header + the file's PCM ---- */

static uint8_t *s_body;
static size_t s_body_len, s_body_off;

static bool load_note(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    static uint8_t file[VP_NOTE_MAX_BYTES * 2];
    size_t n = fread(file, 1, sizeof(file), f);
    fclose(f);
    if (n < 12 || memcmp(file, "RIFF", 4) || memcmp(file + 8, "WAVE", 4)) {
        return false;
    }
    /* find "fmt " (check 16 kHz mono 16-bit) and "data" */
    size_t off = 12, pcm_off = 0, pcm_len = 0;
    bool fmt_ok = false;
    while (off + 8 <= n) {
        uint32_t sz = (uint32_t)file[off + 4] | (uint32_t)file[off + 5] << 8 | (uint32_t)file[off + 6] << 16 |
                      (uint32_t)file[off + 7] << 24;
        if (!memcmp(file + off, "fmt ", 4) && off + 8 + 16 <= n) {
            const uint8_t *p = file + off + 8;
            fmt_ok = p[0] == 1 && p[2] == 1 && (p[4] | p[5] << 8 | p[6] << 16) == 16000 && p[14] == 16;
        } else if (!memcmp(file + off, "data", 4)) {
            pcm_off = off + 8;
            pcm_len = sz > n - pcm_off ? n - pcm_off : sz;
            break;
        }
        off += 8 + sz + (sz & 1);
    }
    if (!fmt_ok || !pcm_len) {
        fprintf(stderr, "note must be 16 kHz mono PCM16 WAV\n");
        return false;
    }
    if (pcm_len + VP_WAV_HEADER > VP_NOTE_MAX_BYTES) {
        pcm_len = (VP_NOTE_MAX_BYTES - VP_WAV_HEADER) & ~(size_t)1;
    }
    s_body = malloc(VP_WAV_HEADER + pcm_len);
    vp_wav_header(s_body, 16000);
    memcpy(s_body + VP_WAV_HEADER, file + pcm_off, pcm_len);
    s_body_len = VP_WAV_HEADER + pcm_len;
    return true;
}

/* 640 bytes = one 20 ms mic chunk at a time, like the device */
static size_t read_body(char *buf, size_t size, size_t nitems, void *ud)
{
    (void)ud;
    size_t room = size * nitems, left = s_body_len - s_body_off;
    size_t n = left < room ? left : room;
    n = n > 640 ? 640 : n;
    memcpy(buf, s_body + s_body_off, n);
    s_body_off += n;
    return n;
}

/* ---- response ---- */

typedef struct {
    vp_turn_t turn;
    char caption[VP_TEXT_MAX];
    char audio_url[VP_URL_MAX];
    bool got_audio;
    long status;
    char content_type[96];
    char retry_after[16];
    char err_body[512];
    size_t err_len;
    double t0, t_first_text;
    size_t sse_bytes;
} turn_ctx_t;

static void redact_url(const char *url, char *out, size_t cap)
{
    /* keep everything up to the id, then the id's first 4 characters */
    const char *slash = strrchr(url, '/');
    if (!slash) {
        snprintf(out, cap, "(?)");
        return;
    }
    snprintf(out, cap, "%.*s/%.4s...mp3", (int)(slash - url), url, slash + 1);
}

static void cb_transcript(void *ctx, const char *text)
{
    (void)ctx;
    printf("  heard:      \"%s\"\n", text);
}

static void cb_start(void *ctx, const char *id)
{
    turn_ctx_t *t = ctx;
    t->caption[0] = '\0';
    printf("  message:    %s\n", id);
}

static void cb_delta(void *ctx, const char *id, const char *text)
{
    turn_ctx_t *t = ctx;
    (void)id;
    if (!t->t_first_text) {
        t->t_first_text = now_s();
    }
    vp_utf8_append(t->caption, sizeof(t->caption), text);
    printf("  caption:    \"%s\"\n", t->caption);
}

static void cb_mdone(void *ctx, const char *id, const char *url)
{
    turn_ctx_t *t = ctx;
    char red[VP_URL_MAX];
    if (url) {
        snprintf(t->audio_url, sizeof(t->audio_url), "%s", url);
        t->got_audio = true;
        redact_url(url, red, sizeof(red));
    }
    printf("  done:       %s, audio -> %s (TTS slot)\n", id, url ? red : "none (caption over silence)");
}

static void cb_error(void *ctx, const char *code, const char *caption)
{
    (void)ctx;
    printf("  ERROR:      %s: \"%s\"\n", code, caption);
}

static void cb_done(void *ctx, bool ok)
{
    (void)ctx;
    printf("  end:        ok=%s\n", ok ? "true" : "false");
}

static const vp_turn_cbs_t CBS = { cb_transcript, cb_start, cb_delta, cb_mdone, cb_error, cb_done };

static size_t on_header(char *buf, size_t size, size_t nitems, void *ud)
{
    turn_ctx_t *t = ud;
    size_t n = size * nitems;
    char line[256];
    size_t l = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
    memcpy(line, buf, l);
    line[l] = '\0';
    while (l && (line[l - 1] == '\r' || line[l - 1] == '\n')) {
        line[--l] = '\0';
    }
    if (!strncasecmp(line, "content-type:", 13)) {
        const char *v = line + 13;
        while (*v == ' ') {
            v++;
        }
        snprintf(t->content_type, sizeof(t->content_type), "%s", v);
    } else if (!strncasecmp(line, "retry-after:", 12)) {
        const char *v = line + 12;
        while (*v == ' ') {
            v++;
        }
        snprintf(t->retry_after, sizeof(t->retry_after), "%s", v);
    }
    return n;
}

static size_t on_body(char *buf, size_t size, size_t nitems, void *ud)
{
    turn_ctx_t *t = ud;
    size_t n = size * nitems;
    if (t->content_type[0] && !strncasecmp(t->content_type, "text/event-stream", 17)) {
        /* feed in odd-sized pieces, to exercise reads split anywhere */
        for (size_t off = 0; off < n;) {
            size_t k = n - off < 37 ? n - off : 37;
            vp_turn_feed(&t->turn, buf + off, k);
            off += k;
        }
        t->sse_bytes += n;
    } else {
        size_t k = n < sizeof(t->err_body) - 1 - t->err_len ? n : sizeof(t->err_body) - 1 - t->err_len;
        memcpy(t->err_body + t->err_len, buf, k);
        t->err_len += k;
        t->err_body[t->err_len] = '\0';
    }
    return n;
}

typedef struct {
    uint8_t head[4];
    size_t got;
} fetched_t;

static size_t count_body(char *buf, size_t size, size_t nitems, void *ud)
{
    size_t n = size * nitems;
    fetched_t *f = ud;
    for (size_t i = 0; i < n && f->got + i < sizeof(f->head); i++) {
        f->head[f->got + i] = (uint8_t)buf[i];
    }
    f->got += n;
    return n;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <base-url> <note.wav>\n", argv[0]);
        return 2;
    }
    const char *token = getenv("VESPER_NODE_TOKEN");
    vp_url_t base;
    if (!vp_url_parse(argv[1], &base)) {
        fprintf(stderr, "bad base url\n");
        return 2;
    }
    char auth[VP_AUTH_MAX];
    vp_header_t hdrs[VP_TURN_HEADERS];
    if (!vp_turn_headers(token, NODE_ID, auth, hdrs)) {
        fprintf(stderr, "VESPER_NODE_TOKEN missing or malformed\n");
        return 2;
    }
    if (!load_note(argv[2])) {
        fprintf(stderr, "can't read the note\n");
        return 2;
    }
    char turn_url[VP_URL_MAX];
    vp_url_join(&base, "/turn", turn_url, sizeof(turn_url));
    printf("POST %s  (%zu byte note = %.2f s, chunked, X-Node-Id %s)\n", turn_url, s_body_len,
           (double)(s_body_len - VP_WAV_HEADER) / 32000.0, NODE_ID);

    setvbuf(stdout, NULL, _IOLBF, 0);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    static turn_ctx_t t;
    int rc = 1;
    for (int attempt = 0; attempt < 2; attempt++) {
        memset(&t, 0, sizeof(t));
        vp_turn_init(&t.turn, &base, &CBS, &t);
        s_body_off = 0;
        struct curl_slist *list = NULL;
        char line[VP_AUTH_MAX + 64];
        for (int i = 0; i < VP_TURN_HEADERS; i++) {
            snprintf(line, sizeof(line), "%s: %s", hdrs[i].name, hdrs[i].value);
            list = curl_slist_append(list, line);
        }
        list = curl_slist_append(list, "Transfer-Encoding: chunked");
        list = curl_slist_append(list, "Expect:");
        memset(line, 0, sizeof(line));
        CURL *c = curl_easy_init();
        curl_easy_setopt(c, CURLOPT_URL, turn_url);
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);
        curl_easy_setopt(c, CURLOPT_READFUNCTION, read_body);
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, &t);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &t);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 90L);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
        t.t0 = now_s();
        CURLcode res = curl_easy_perform(c);
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &t.status);
        curl_easy_cleanup(c);
        curl_slist_free_all(list);
        double t_end = now_s();

        char err_code[VP_CODE_MAX] = "";
        if (t.err_len) {
            vp_json_string(t.err_body, t.err_len, "error", err_code, sizeof(err_code));
        }
        int retry_ms;
        char cap[48];
        vp_http_verdict_t v = vp_http_verdict(res == CURLE_OK ? (int)t.status : 0, t.content_type, err_code,
                                              t.retry_after, attempt, &retry_ms, cap, sizeof(cap));
        printf("HTTP %ld %s\n", t.status, t.content_type);
        if (v == VP_HTTP_RETRY) {
            printf("  %s (%d ms)\n", cap, retry_ms);
            struct timespec ts = { retry_ms / 1000, (retry_ms % 1000) * 1000000L };
            nanosleep(&ts, NULL);
            continue;
        }
        if (v == VP_HTTP_FAIL) {
            printf("  caption: %s (error %s)\n", cap, err_code[0] ? err_code : "-");
            break;
        }
        bool ended = vp_turn_finish(&t.turn);
        printf("stream: %zu bytes, %u message(s), %u unknown event(s), %u malformed, %u dropped, ended=%s\n",
               t.sse_bytes, t.turn.messages, t.turn.unknown_events, t.turn.bad_events, t.turn.sse.dropped,
               ended ? "done" : "CUT");
        printf("latency: first caption %.2f s after upload start, stream closed at %.2f s\n",
               t.t_first_text ? t.t_first_text - t.t0 : -1.0, t_end - t.t0);
        printf("FINAL CAPTION: \"%s\"\n", t.caption);
        rc = ended && !t.turn.errored && t.caption[0] ? 0 : 1;

        if (t.got_audio) {
            /* What task 10 will do: GET the resolved URL with the same bearer. */
            fetched_t fetched = { { 0 }, 0 };
            CURL *g = curl_easy_init();
            char authline[VP_AUTH_MAX + 32];
            snprintf(authline, sizeof(authline), "Authorization: %s", hdrs[0].value);
            struct curl_slist *gl = curl_slist_append(NULL, authline);
            memset(authline, 0, sizeof(authline));
            curl_easy_setopt(g, CURLOPT_URL, t.audio_url);
            curl_easy_setopt(g, CURLOPT_HTTPHEADER, gl);
            curl_easy_setopt(g, CURLOPT_WRITEFUNCTION, count_body);
            curl_easy_setopt(g, CURLOPT_WRITEDATA, &fetched);
            curl_easy_setopt(g, CURLOPT_TIMEOUT, 20L);
            CURLcode gr = curl_easy_perform(g);
            long gs = 0;
            curl_easy_getinfo(g, CURLINFO_RESPONSE_CODE, &gs);
            curl_easy_cleanup(g);
            curl_slist_free_all(gl);
            const uint8_t *head = fetched.head;
            bool mp3 = (head[0] == 'I' && head[1] == 'D' && head[2] == '3') || (head[0] == 0xFF && (head[1] & 0xE0) == 0xE0);
            printf("audio GET: HTTP %ld, %zu bytes, %s\n", gs, fetched.got, gr == CURLE_OK && mp3 ? "MP3" : "NOT MP3");
            if (gr != CURLE_OK || gs != 200 || !mp3) {
                rc = 1;
            }
        }
        break;
    }
    memset(auth, 0, sizeof(auth));
    free(s_body);
    curl_global_cleanup();
    return rc;
}
