/*
 * The Vesper backend of the muse_hatch_* seam: each push-to-talk turn is one
 * POST <host>/turn to the Vesper node backend (docs/node-wire-protocol.md v1).
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project. The reply pacing, caption paging and MP3 self-test follow the
 * stock muse_chat_session.cpp (Meta Platforms, Apache-2.0) so the voice task
 * (muse_voice.c, unchanged) sees exactly the behaviour it was written for;
 * those functions say so.
 *
 * A turn:
 *   1. Press (turn_begin): open the POST with Transfer-Encoding: chunked and
 *      send the streaming WAV header, so the TLS handshake and the upload
 *      overlap the speech. Mic audio is streamed up in ~4 KB chunks while the
 *      button is held, and also kept (<= 512 KiB) for the one retry the
 *      protocol allows (503 busy).
 *   2. Release (turn_end): send the last chunk and the terminating chunk,
 *      read the status. Anything but 200 text/event-stream becomes a short
 *      error caption (vp_http_verdict); 503 is retried once after Retry-After
 *      with the kept note and a Content-Length.
 *   3. Reply: the SSE stream is read in short polls on this task and parsed
 *      by vesper_proto.c: transcript -> HEARD, text deltas -> captions,
 *      message_done -> the TTS slot (vesper_tts_slot_offer, task 10), error ->
 *      ERROR, done -> end of stream. As in stock firmware, each finished
 *      message's caption is paced over silence (16 chars/s) so the voice task
 *      pages it, and the turn ends (DONE) once every message has been shown.
 *
 * Everything network-side runs on one task. The voice task talks to it through
 * a command queue, a stream buffer of mic audio, an event queue (captions) and
 * a stream buffer of reply audio, exactly as with the stock backend. A
 * generation number tags each turn so that events of a cancelled turn are
 * dropped. No Meta account, token or transport is involved.
 */
#include "muse_chat_vesper.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "minimp3.h"
#include "muse_chat_priv.h"
#include "muse_settings.h"
#include "muse_wifi.h"
#include "vesper_proto.h"

static const char *TAG = "vesper_chat";

#define MIC_RATE 16000
#define IN_BYTES (MIC_RATE * 2 * 8)        /* 8 s of mic backlog while connecting */
#define OUT_BYTES (MIC_RATE * 2 * 2)       /* 2 s of reply audio (silence, until task 10) */
#define STAGE_BYTES 4096                   /* PCM per upload chunk (128 ms) */
#define EV_TEXT 72
#define MAX_MSGS 4
#define TEXT_MAX 1024                      /* a message's text, for captions timed to its speech */
#define SPEECH_CHARS_PER_S 14              /* until the speech's length is known */
#define TEXT_CHARS_PER_S 16                /* reading pace, a little over speech */
#define TEXT_HOLD_S 2                      /* how long a message's last lines stay up */
#define MIN_NOTE_S 0.3

#define CONNECT_TIMEOUT_MS 10000           /* DNS + TCP + TLS, and each upload write */
#define HEADERS_TIMEOUT_MS 15000           /* release -> response status */
#define POLL_MS 40                         /* SSE read poll while pacing captions */
#define STREAM_IDLE_US (60 * 1000000LL)    /* nothing (not even a ping) for this long: give up */
#define TURN_CAP_US (180 * 1000000LL)
#define ERR_BODY_MAX 256

/* ---- Voice task <-> hatch task ---- */

typedef enum { CMD_CONNECT, CMD_FORGET, CMD_BEGIN, CMD_END, CMD_CANCEL, CMD_WAKE } cmd_type_t;

typedef struct {
    cmd_type_t type;
    uint32_t gen;
} cmd_t;

typedef struct {
    muse_hatch_ev_t type;
    uint32_t gen;
    char text[EV_TEXT];
} ev_t;

static QueueHandle_t s_cmds, s_events;
static StreamBufferHandle_t s_in, s_out;
static atomic_uint s_gen;
static atomic_bool s_resting;
static char s_node_id[VP_NODE_ID_MAX + 1];

/* ---- The current turn ---- */

typedef enum { P_IDLE, P_LISTEN, P_STREAM, P_PACE } phase_t;
typedef enum { TTS_NONE, TTS_QUEUED, TTS_ACTIVE, TTS_FINISHED } tts_t;

typedef struct {
    size_t len;              /* reply text length so far */
    bool done;
    tts_t tts;
    uint32_t pcm_start;      /* where its (silent) speech starts in the reply audio */
    uint32_t pcm_frames;
} msg_t;

typedef struct {
    phase_t phase;
    uint32_t gen;
    esp_http_client_handle_t http;
    vp_url_t base;
    char url[VP_URL_MAX];
    char auth[VP_AUTH_MAX];
    vp_header_t hdrs[VP_TURN_HEADERS];
    bool end_requested;
    bool upload_broken;      /* a write failed: the server may have answered early */
    int attempt;
    int64_t start_us, release_us, last_rx_us;
    uint8_t *note;           /* VP_NOTE_MAX_BYTES: WAV header + PCM, kept for the retry */
    size_t note_len;
    size_t staged_from;      /* note[staged_from..note_len) isn't uploaded yet */
    uint8_t *chunk;          /* chunk framing + STAGE_BYTES + CRLF */
    /* response headers (HTTP_EVENT_ON_HEADER) */
    char content_type[64];
    char retry_after[16];
    /* reply */
    vp_turn_t sse;
    char *texts;             /* MAX_MSGS * TEXT_MAX */
    msg_t msgs[MAX_MSGS];
    int nmsgs;
    int cur;                 /* message receiving deltas, or -1 */
    bool stream_done;        /* the `done` event arrived */
    bool failed;
    int tts_msg;             /* message being shown, or -1 */
    uint32_t pcm_out;        /* reply audio frames handed to the voice task */
    char reply_shown[EV_TEXT];
    int64_t t_status, t_text, t_done;
} turn_t;

/* ~22 KB, most of it the SSE parser: PSRAM on the AIPI, like the stock turn state. */
EXT_RAM_BSS_ATTR static turn_t s_turn;
static uint8_t s_rx[1024];

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static void *psram_alloc(size_t n)
{
    return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

/* ---- Events to the voice task ---- */

static void emit(muse_hatch_ev_t type, const char *text)
{
    ev_t ev = { .type = type, .gen = s_turn.gen };
    if (type == MUSE_HATCH_EV_HEARD) {
        char tail[EV_TEXT];
        muse_hatch_tail_words(text ? text : "", tail, sizeof(tail));
        vp_utf8_copy(ev.text, sizeof(ev.text), tail, strlen(tail));
    } else if (text) {
        vp_utf8_copy(ev.text, sizeof(ev.text), text, strlen(text));
    }
    /* Captions are lossy; the end of a turn, and whether the note got there, must get through. */
    TickType_t wait = (type == MUSE_HATCH_EV_DONE || type == MUSE_HATCH_EV_ERROR || type == MUSE_HATCH_EV_SENT)
                          ? pdMS_TO_TICKS(200)
                          : 0;
    xQueueSend(s_events, &ev, wait);
}

/* ---- HTTP ---- */

static esp_err_t on_http_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value) {
        if (!strcasecmp(e->header_key, "Content-Type")) {
            strlcpy(s_turn.content_type, e->header_value, sizeof(s_turn.content_type));
        } else if (!strcasecmp(e->header_key, "Retry-After")) {
            strlcpy(s_turn.retry_after, e->header_value, sizeof(s_turn.retry_after));
        }
    }
    return ESP_OK;
}

static void http_close(void)
{
    if (s_turn.http) {
        esp_http_client_close(s_turn.http);
        esp_http_client_cleanup(s_turn.http);
        s_turn.http = NULL;
    }
}

/* Opens POST <host>/turn: content_length < 0 sends the body chunked. */
static bool http_open(int content_length)
{
    esp_http_client_config_t cfg = {
        .url = s_turn.url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CONNECT_TIMEOUT_MS,
        .event_handler = on_http_event,
        .buffer_size = 1024,
        .buffer_size_tx = VP_AUTH_MAX + 512,   /* the request line and headers, bearer included */
        .disable_auto_redirect = true,         /* never carry the bearer elsewhere */
        .crt_bundle_attach = s_turn.base.https ? esp_crt_bundle_attach : NULL,
    };
    s_turn.content_type[0] = s_turn.retry_after[0] = '\0';
    s_turn.http = esp_http_client_init(&cfg);
    if (!s_turn.http) {
        return false;
    }
    for (int i = 0; i < VP_TURN_HEADERS; i++) {
        esp_http_client_set_header(s_turn.http, s_turn.hdrs[i].name, s_turn.hdrs[i].value);
    }
    esp_err_t err = esp_http_client_open(s_turn.http, content_length);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
        http_close();
        return false;
    }
    return true;
}

static bool write_all(const void *data, size_t len)
{
    const char *p = data;
    while (len) {
        int n = esp_http_client_write(s_turn.http, p, (int)len);
        if (n <= 0) {
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

/* One chunk of a chunked body; len 0 is the terminating chunk. */
static bool write_chunk(const uint8_t *data, size_t len)
{
    if (s_turn.upload_broken) {
        return false;
    }
    uint8_t *c = s_turn.chunk;
    int h = snprintf((char *)c, 16, "%x\r\n", (unsigned)len);
    memcpy(c + h, data, len);
    size_t n = (size_t)h + len;
    memcpy(c + n, "\r\n", 2);
    n += 2;
    if (!write_all(c, n)) {
        ESP_LOGW(TAG, "upload write failed after %u bytes", (unsigned)s_turn.staged_from);
        s_turn.upload_broken = true;
        return false;
    }
    return true;
}

/* ---- Turn bookkeeping ---- */

static void report_result(muse_hatch_state_t state, const char *detail)
{
    muse_hatch_report(state, detail);
}

static void turn_finish(void)
{
    http_close();
    s_turn.phase = P_IDLE;
    s_turn.tts_msg = -1;
    memset(s_turn.auth, 0, sizeof(s_turn.auth));
}

static void turn_fail(const char *why)
{
    ESP_LOGW(TAG, "turn failed: %s", why);
    s_turn.failed = true;
    emit(MUSE_HATCH_EV_ERROR, why);
    turn_finish();
}

static void log_timing(void)
{
    int64_t r = s_turn.release_us;
    ESP_LOGI(TAG, "timing (ms after release): status +%d text +%d done +%d",
             s_turn.t_status && r ? (int)((s_turn.t_status - r) / 1000) : -1,
             s_turn.t_text && r ? (int)((s_turn.t_text - r) / 1000) : -1,
             s_turn.t_done && r ? (int)((s_turn.t_done - r) / 1000) : -1);
}

/* ---- Reply callbacks (vesper_proto.c, on this task) ---- */

static char *msg_text(int i)
{
    return s_turn.texts + i * TEXT_MAX;
}

/*
 * Shows a reply that hasn't started "speaking": its opening lines, so they can
 * be read while the rest arrives. Adapted from muse_chat_session.cpp.
 */
static void show_reply_start(int i)
{
    char line[EV_TEXT];
    if (!muse_hatch_caption_at(msg_text(i), 0, line, sizeof(line))) {
        return;
    }
    if (strcmp(line, s_turn.reply_shown) != 0) {
        strlcpy(s_turn.reply_shown, line, sizeof(s_turn.reply_shown));
        emit(MUSE_HATCH_EV_REPLY, line);
    }
}

static void on_transcript(void *ctx, const char *text)
{
    (void)ctx;
    ESP_LOGI(TAG, "transcript: %u chars", (unsigned)strlen(text));   /* never the text itself */
    if (text[0]) {
        emit(MUSE_HATCH_EV_HEARD, text);
    }
}

static void on_message_start(void *ctx, const char *id)
{
    (void)ctx;
    (void)id;
    if (s_turn.nmsgs == MAX_MSGS) {
        s_turn.cur = -1;   /* more than a node shows; the rest is dropped */
        return;
    }
    s_turn.cur = s_turn.nmsgs++;
    msg_t *m = &s_turn.msgs[s_turn.cur];
    memset(m, 0, sizeof(*m));
    msg_text(s_turn.cur)[0] = '\0';
}

static void on_text_delta(void *ctx, const char *id, const char *text)
{
    (void)ctx;
    (void)id;
    if (s_turn.cur < 0) {
        return;
    }
    msg_t *m = &s_turn.msgs[s_turn.cur];
    char *full = msg_text(s_turn.cur);
    vp_utf8_append(full, TEXT_MAX, text);
    m->len = strlen(full);
    if (!s_turn.t_text) {
        s_turn.t_text = now_us();
    }
    if (s_turn.tts_msg < 0) {
        show_reply_start(s_turn.cur);
    }
}

/* Weak default for the task-10 slot: no playback yet, captions over silence. */
__attribute__((weak)) bool vesper_tts_slot_offer(const char *abs_url, int msg)
{
    (void)abs_url;
    ESP_LOGI(TAG, "message %d has speech; TTS slot empty (task 10), showing the caption", msg);
    return false;
}

static void on_message_done(void *ctx, const char *id, const char *audio_url)
{
    (void)ctx;
    (void)id;
    if (s_turn.cur < 0) {
        return;
    }
    int i = s_turn.cur;
    msg_t *m = &s_turn.msgs[i];
    s_turn.cur = -1;
    m->done = true;
    s_turn.t_done = now_us();
    ESP_LOGI(TAG, "message %d done (%u chars, %s)", i, (unsigned)m->len, audio_url ? "with speech" : "no speech");
    if (!m->len) {
        m->tts = TTS_FINISHED;   /* nothing to show */
        return;
    }
    if (audio_url && vesper_tts_slot_offer(audio_url, i)) {
        m->tts = TTS_ACTIVE;     /* the slot owns it now */
        return;
    }
    m->tts = TTS_QUEUED;
}

static void on_error(void *ctx, const char *code, const char *caption)
{
    (void)ctx;
    ESP_LOGW(TAG, "server error event: %s", code[0] ? code : "(none)");
    report_result(MUSE_HATCH_REACHABLE, "Connected");
    turn_fail(caption);
}

static void on_done(void *ctx, bool ok)
{
    (void)ctx;
    s_turn.stream_done = true;
    ESP_LOGI(TAG, "reply stream done (ok=%d, %d message(s))", ok, s_turn.nmsgs);
}

static const vp_turn_cbs_t CBS = {
    .transcript = on_transcript,
    .message_start = on_message_start,
    .text_delta = on_text_delta,
    .message_done = on_message_done,
    .error = on_error,
    .done = on_done,
};

/* ---- Turn: upload ---- */

static bool load_config(void)
{
    static char host[MUSE_HOST_MAX + 1];
    static char token[MUSE_TOKEN_MAX + 1];
    muse_settings_hatch_host(host);
    muse_settings_hatch_token(token);
    bool ok = vp_url_parse(host, &s_turn.base) && vp_url_join(&s_turn.base, "/turn", s_turn.url, sizeof(s_turn.url)) &&
              vp_turn_headers(token, s_node_id, s_turn.auth, s_turn.hdrs);
    memset(token, 0, sizeof(token));
    return ok;
}

static void turn_begin(uint32_t gen)
{
    if (s_turn.phase != P_IDLE) {
        turn_finish();
    }
    uint8_t *note = s_turn.note, *chunk = s_turn.chunk;
    char *texts = s_turn.texts;
    memset(&s_turn, 0, offsetof(turn_t, sse));
    memset(s_turn.msgs, 0, sizeof(s_turn.msgs));
    s_turn.note = note;
    s_turn.chunk = chunk;
    s_turn.texts = texts;
    s_turn.nmsgs = 0;
    s_turn.cur = s_turn.tts_msg = -1;
    s_turn.stream_done = s_turn.failed = false;
    s_turn.pcm_out = 0;
    s_turn.reply_shown[0] = '\0';
    s_turn.t_status = s_turn.t_text = s_turn.t_done = 0;
    s_turn.gen = gen;
    s_turn.start_us = now_us();

    if (!muse_hatch_configured() || !load_config()) {
        turn_fail("SET UP VESPER FIRST");
        return;
    }
    if (!muse_wifi_connected()) {
        turn_fail("NO WI-FI");
        return;
    }
    vp_turn_init(&s_turn.sse, &s_turn.base, &CBS, NULL);
    muse_hatch_wav_header(s_turn.note, MIC_RATE);   /* the stock helper (muse_chat_text.c) */
    s_turn.note_len = VP_WAV_HEADER;
    s_turn.staged_from = 0;
    if (!http_open(-1)) {
        report_result(MUSE_HATCH_UNREACHABLE, "Can't connect");
        turn_fail("CAN'T REACH VESPER");
        return;
    }
    s_turn.phase = P_LISTEN;
}

/* Sends what's staged as one chunk (all of it if `all`, else whole STAGE_BYTES only). */
static void flush_stage(bool all)
{
    while (s_turn.note_len - s_turn.staged_from >= STAGE_BYTES ||
           (all && s_turn.note_len > s_turn.staged_from)) {
        size_t n = s_turn.note_len - s_turn.staged_from;
        n = n > STAGE_BYTES ? STAGE_BYTES : n;
        write_chunk(s_turn.note + s_turn.staged_from, n);   /* a failure is read at the release */
        s_turn.staged_from += n;
    }
}

static void read_error_body(char *code, size_t cap)
{
    char body[ERR_BODY_MAX];
    int got = 0;
    code[0] = '\0';
    while (got < (int)sizeof(body) - 1) {
        int n = esp_http_client_read(s_turn.http, body + got, (int)sizeof(body) - 1 - got);
        if (n <= 0) {
            break;
        }
        got += n;
    }
    if (got > 0 && vp_json_string(body, (size_t)got, "error", code, cap) != VP_JSON_STRING) {
        code[0] = '\0';
    }
}

/* After the last byte of the note: the status, and what to do about it. */
static void await_status(void)
{
    for (;;) {
        esp_http_client_set_timeout_ms(s_turn.http, HEADERS_TIMEOUT_MS);
        int64_t r = esp_http_client_fetch_headers(s_turn.http);
        int status = r < 0 ? 0 : esp_http_client_get_status_code(s_turn.http);
        s_turn.t_status = now_us();
        char code[VP_CODE_MAX] = "";
        if (status && status != 200) {
            read_error_body(code, sizeof(code));
        }
        int retry_ms = 0;
        char caption[EV_TEXT];
        vp_http_verdict_t v = vp_http_verdict(status, s_turn.content_type, code, s_turn.retry_after, s_turn.attempt,
                                              &retry_ms, caption, sizeof(caption));
        ESP_LOGI(TAG, "turn: HTTP %d%s%s", status, code[0] ? " " : "", code);
        if (v == VP_HTTP_STREAM) {
            report_result(MUSE_HATCH_REACHABLE, "Connected");
            emit(MUSE_HATCH_EV_SENT, NULL);
            esp_http_client_set_timeout_ms(s_turn.http, POLL_MS);
            s_turn.last_rx_us = now_us();
            s_turn.phase = P_STREAM;
            return;
        }
        if (v == VP_HTTP_FAIL) {
            report_result(status == 0 || status == 401 ? MUSE_HATCH_UNREACHABLE : MUSE_HATCH_REACHABLE,
                          status == 401 ? "Token refused" : status == 0 ? "Can't connect" : "Connected");
            turn_fail(caption);
            return;
        }
        /* 503 busy: once more after Retry-After, the kept note in one piece */
        http_close();
        ESP_LOGI(TAG, "server busy; retrying in %d ms", retry_ms);
        int64_t until = now_us() + (int64_t)retry_ms * 1000;
        while (now_us() < until) {
            cmd_t cmd;
            if (xQueuePeek(s_cmds, &cmd, pdMS_TO_TICKS(50)) == pdTRUE && cmd.type == CMD_CANCEL &&
                cmd.gen == s_turn.gen) {
                turn_finish();   /* the queue's CMD_CANCEL then finds the turn idle */
                return;
            }
        }
        s_turn.attempt++;
        s_turn.upload_broken = false;
        if (!load_config() || !http_open((int)s_turn.note_len) || !write_all(s_turn.note, s_turn.note_len)) {
            report_result(MUSE_HATCH_UNREACHABLE, "Can't connect");
            turn_fail("CAN'T REACH VESPER");
            return;
        }
    }
}

/* Moves the mic into the note request; on release, ends the body and reads the status. */
static void record_note(void)
{
    for (;;) {
        size_t room = (VP_NOTE_MAX_BYTES - s_turn.note_len) & ~(size_t)1;
        size_t got = room ? xStreamBufferReceive(s_in, s_turn.note + s_turn.note_len,
                                                 room < STAGE_BYTES ? room : STAGE_BYTES, 0)
                          : 0;
        if (!got) {
            if (!room) {
                /* full (16 s): drop the rest, as the stock note stops at 20 s */
                uint8_t junk[256];
                while (xStreamBufferReceive(s_in, junk, sizeof(junk), 0)) {
                }
            }
            break;
        }
        s_turn.note_len += got;
        flush_stage(false);
    }
    if (!s_turn.end_requested) {
        return;
    }
    s_turn.release_us = now_us();
    size_t pcm = s_turn.note_len - VP_WAV_HEADER;
    double secs = (double)pcm / (MIC_RATE * 2);
    if (secs < MIN_NOTE_S) {
        turn_fail("DIDN'T CATCH THAT");   /* closing the connection abandons the request */
        return;
    }
    flush_stage(true);
    write_chunk(NULL, 0);
    ESP_LOGI(TAG, "voice note: %.2fs, %u bytes%s", secs, (unsigned)s_turn.note_len,
             s_turn.upload_broken ? " (upload cut short)" : "");
    await_status();
}

/* ---- Turn: reply ---- */

/* Picks the next finished message to show. Adapted from start_tts() in muse_chat_session.cpp. */
static void start_showing(void)
{
    if (s_turn.tts_msg >= 0) {
        return;
    }
    for (int i = 0; i < s_turn.nmsgs; i++) {
        msg_t *m = &s_turn.msgs[i];
        if (m->tts != TTS_QUEUED) {
            continue;
        }
        m->pcm_start = s_turn.pcm_out;
        m->pcm_frames = (uint32_t)(m->len * MIC_RATE / TEXT_CHARS_PER_S);
        m->tts = TTS_ACTIVE;
        s_turn.tts_msg = i;
        show_reply_start(i);
        return;
    }
}

/* Queues the shown message's silence while the reply buffer has room. From muse_chat_session.cpp. */
static void pace_silently(void)
{
    static const int16_t zeros[256];
    if (s_turn.tts_msg < 0) {
        return;
    }
    msg_t *m = &s_turn.msgs[s_turn.tts_msg];
    uint32_t end = m->pcm_start + m->pcm_frames + TEXT_HOLD_S * MIC_RATE;
    while (s_turn.pcm_out < end && xStreamBufferSpacesAvailable(s_out) >= sizeof(zeros)) {
        uint32_t n = end - s_turn.pcm_out < 256 ? end - s_turn.pcm_out : 256;
        xStreamBufferSend(s_out, zeros, n * sizeof(int16_t), 0);
        s_turn.pcm_out += n;
    }
    if (s_turn.pcm_out >= end) {
        m->tts = TTS_FINISHED;
        s_turn.tts_msg = -1;
    }
}

static void end_stream(void)
{
    bool ended = vp_turn_finish(&s_turn.sse);
    http_close();
    if (s_turn.phase == P_IDLE) {
        return;   /* an error event already ended the turn */
    }
    if (!ended) {
        bool have_text = false;
        for (int i = 0; i < s_turn.nmsgs; i++) {
            have_text = have_text || s_turn.msgs[i].len;
            if (s_turn.msgs[i].len && !s_turn.msgs[i].done) {
                s_turn.msgs[i].done = true;   /* show what did arrive */
                s_turn.msgs[i].tts = TTS_QUEUED;
            }
        }
        ESP_LOGW(TAG, "reply stream cut short (%d message(s))", s_turn.nmsgs);
        if (!have_text) {
            turn_fail("LOST CONNECTION TO VESPER");
            return;
        }
    }
    if (s_turn.sse.sse.dropped || s_turn.sse.bad_events) {
        ESP_LOGW(TAG, "reply: %u event(s) dropped, %u malformed", s_turn.sse.sse.dropped, s_turn.sse.bad_events);
    }
    s_turn.phase = P_PACE;
}

static void pump_stream(void)
{
    for (int reads = 0; reads < 8 && s_turn.phase == P_STREAM; reads++) {
        int n = esp_http_client_read(s_turn.http, (char *)s_rx, sizeof(s_rx));
        if (n > 0) {
            s_turn.last_rx_us = now_us();
            vp_turn_feed(&s_turn.sse, (const char *)s_rx, (size_t)n);
            if (s_turn.phase == P_STREAM && s_turn.stream_done) {
                end_stream();
            }
            continue;
        }
        if (n == -ESP_ERR_HTTP_EAGAIN) {
            if (now_us() - s_turn.last_rx_us > STREAM_IDLE_US) {
                http_close();
                turn_fail("VESPER TIMED OUT");
            }
            return;
        }
        end_stream();   /* 0: the body ended; < 0: the connection dropped */
    }
}

/* Ends the turn once every message has been shown. */
static void check_turn(void)
{
    if (now_us() - s_turn.start_us > TURN_CAP_US) {
        ESP_LOGW(TAG, "turn hit the time cap");
        http_close();
        emit(MUSE_HATCH_EV_DONE, NULL);
        turn_finish();
        return;
    }
    if (s_turn.phase != P_PACE) {
        return;
    }
    for (int i = 0; i < s_turn.nmsgs; i++) {
        if (s_turn.msgs[i].tts == TTS_QUEUED || s_turn.msgs[i].tts == TTS_ACTIVE) {
            return;
        }
    }
    ESP_LOGI(TAG, "turn done: %d message(s) in %.1fs", s_turn.nmsgs, (now_us() - s_turn.start_us) / 1e6);
    log_timing();
    emit(MUSE_HATCH_EV_DONE, NULL);
    turn_finish();
}

/* ---- Reachability check (settings "test", serial >hatch.test) ---- */

static void probe(void)
{
    if (!muse_hatch_configured() || !muse_wifi_connected()) {
        report_result(MUSE_HATCH_UNTESTED, "");
        return;
    }
    char url[VP_URL_MAX];
    if (!load_config() || !vp_url_join(&s_turn.base, "/healthz", url, sizeof(url))) {
        report_result(MUSE_HATCH_UNREACHABLE, "Bad server URL");
        return;
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = CONNECT_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .buffer_size_tx = VP_AUTH_MAX + 512,
        .crt_bundle_attach = s_turn.base.https ? esp_crt_bundle_attach : NULL,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return;
    }
    /* Through Peggy the bearer is what gets the request past the edge. */
    esp_http_client_set_header(c, s_turn.hdrs[0].name, s_turn.hdrs[0].value);
    esp_err_t err = esp_http_client_perform(c);
    int status = err == ESP_OK ? esp_http_client_get_status_code(c) : 0;
    esp_http_client_cleanup(c);
    memset(s_turn.auth, 0, sizeof(s_turn.auth));
    char detail[48];
    if (status == 200) {
        report_result(MUSE_HATCH_REACHABLE, "Connected");
    } else if (status == 401) {
        report_result(MUSE_HATCH_UNREACHABLE, "Token refused");
    } else if (status) {
        snprintf(detail, sizeof(detail), "Reachable (HTTP %d)", status);
        report_result(MUSE_HATCH_REACHABLE, detail);
    } else {
        report_result(MUSE_HATCH_UNREACHABLE, "Can't connect");
    }
    ESP_LOGI(TAG, "server check: HTTP %d", status);
}

/* ---- Task ---- */

static void handle(const cmd_t *cmd)
{
    switch (cmd->type) {
    case CMD_CONNECT:
        if (s_turn.phase == P_IDLE) {
            probe();
        }
        break;
    case CMD_FORGET:
        if (s_turn.phase != P_IDLE) {
            turn_fail("SETTINGS CHANGED");
        }
        break;
    case CMD_BEGIN:
        if (cmd->gen == atomic_load(&s_gen)) {
            turn_begin(cmd->gen);
        }
        break;
    case CMD_END:
        if (cmd->gen == s_turn.gen && s_turn.phase == P_LISTEN) {
            s_turn.end_requested = true;
        }
        break;
    case CMD_CANCEL:
        if (cmd->gen == s_turn.gen && s_turn.phase != P_IDLE) {
            ESP_LOGI(TAG, "turn cancelled");
            turn_finish();
        }
        break;
    case CMD_WAKE:
        break;
    }
}

static void hatch_task(void *arg)
{
    (void)arg;
    for (;;) {
        cmd_t cmd;
        int wait_ms = s_turn.phase == P_LISTEN ? 5 : s_turn.phase != P_IDLE ? 2 : atomic_load(&s_resting) ? -1 : 500;
        TickType_t wait = wait_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms);
        if (xQueueReceive(s_cmds, &cmd, wait) == pdTRUE) {
            handle(&cmd);
            while (xQueueReceive(s_cmds, &cmd, 0) == pdTRUE) {
                handle(&cmd);
            }
        }
        if (s_turn.phase == P_LISTEN) {
            record_note();
        }
        if (s_turn.phase == P_STREAM) {
            pump_stream();
        }
        if (s_turn.phase == P_STREAM || s_turn.phase == P_PACE) {
            start_showing();
            pace_silently();
        }
        if (s_turn.phase != P_IDLE && s_turn.phase != P_LISTEN) {
            check_turn();
        }
    }
}

/* ---- Public API (muse_chat.h) ---- */

static void post(cmd_type_t type, uint32_t gen)
{
    if (s_cmds) {
        cmd_t cmd = { type, gen };
        xQueueSend(s_cmds, &cmd, pdMS_TO_TICKS(100));
    }
}

static void drain_out(void)
{
    static int16_t junk[256];
    while (xStreamBufferReceive(s_out, junk, sizeof(junk), 0)) {
    }
}

void muse_hatch_set_node_id(const char *node_id)
{
    strlcpy(s_node_id, node_id ? node_id : "", sizeof(s_node_id));
}

void muse_hatch_start(void)
{
    if (s_cmds) {
        return;
    }
    s_cmds = xQueueCreate(16, sizeof(cmd_t));
    s_events = xQueueCreate(16, sizeof(ev_t));
    s_in = xStreamBufferCreateWithCaps(IN_BYTES, 1, MALLOC_CAP_SPIRAM);
    s_out = xStreamBufferCreateWithCaps(OUT_BYTES, 1, MALLOC_CAP_SPIRAM);
    s_turn.note = psram_alloc(VP_NOTE_MAX_BYTES);
    s_turn.chunk = psram_alloc(16 + STAGE_BYTES + 2);
    s_turn.texts = psram_alloc(MAX_MSGS * TEXT_MAX);
    s_turn.cur = s_turn.tts_msg = -1;
    if (!s_node_id[0]) {
        ESP_LOGW(TAG, "no node id set; turns will be refused");
    }
    /* Stack in PSRAM, like the stock session task: TLS runs here (and task 10's MP3 decoder). */
    if (!s_cmds || !s_events || !s_in || !s_out || !s_turn.note || !s_turn.chunk || !s_turn.texts ||
        xTaskCreatePinnedToCoreWithCaps(hatch_task, "muse_chat", 32 * 1024, NULL, 5, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "start failed");
    }
}

void muse_hatch_chat_connect(void)
{
    post(CMD_CONNECT, 0);
}

void muse_hatch_chat_forget(void)
{
    post(CMD_FORGET, 0);
}

bool muse_hatch_ready(void)
{
    return s_cmds && muse_hatch_configured() && muse_wifi_connected();
}

void muse_hatch_turn_begin(void)
{
    uint32_t gen = atomic_fetch_add(&s_gen, 1) + 1;
    xStreamBufferReset(s_in);
    drain_out();
    post(CMD_BEGIN, gen);
}

void muse_hatch_turn_audio(const int16_t *pcm, size_t frames)
{
    size_t bytes = frames * sizeof(int16_t);
    if (xStreamBufferSend(s_in, pcm, bytes, 0) != bytes) {
        ESP_LOGW(TAG, "mic backlog full, dropped audio");
    }
}

size_t muse_hatch_turn_audio_wait(const int16_t *pcm, size_t frames, int wait_ms)
{
    return xStreamBufferSend(s_in, pcm, frames * sizeof(int16_t), pdMS_TO_TICKS(wait_ms)) / sizeof(int16_t);
}

void muse_hatch_turn_end(void)
{
    post(CMD_END, atomic_load(&s_gen));
}

void muse_hatch_turn_cancel(void)
{
    uint32_t gen = atomic_fetch_add(&s_gen, 1);
    post(CMD_CANCEL, gen);
    drain_out();
}

void muse_hatch_set_resting(bool resting)
{
    if (atomic_exchange(&s_resting, resting) && !resting) {
        post(CMD_WAKE, 0);
    }
}

/* Typed console chat rode Meta's /chat/stream; protocol v1 has no text turn. */
void muse_hatch_text_turn(char *text)
{
    free(text);
    muse_hatch_console("error", "TYPED CHAT ISN'T SUPPORTED BY THE VESPER BACKEND", NULL);
}

void muse_hatch_text_cancel(void)
{
}

muse_hatch_ev_t muse_hatch_turn_event(char *text, size_t cap)
{
    ev_t ev;
    while (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
        if (ev.gen == atomic_load(&s_gen)) {
            vp_utf8_copy(text, cap, ev.text, strlen(ev.text));
            return ev.type;
        }
    }
    return MUSE_HATCH_EV_NONE;
}

/* The page of the message being "said" after `played` frames. From muse_chat_session.cpp. */
bool muse_hatch_turn_caption(size_t played, char *out, size_t cap)
{
    const msg_t *m = NULL;
    const char *text = NULL;
    for (int i = 0; i < s_turn.nmsgs && s_turn.texts; i++) {
        const msg_t *c = &s_turn.msgs[i];
        if (c->tts >= TTS_ACTIVE && c->len && c->pcm_start <= played) {
            m = c;
            text = msg_text(i);
        }
    }
    if (!m) {
        /* Nothing said yet: the reply's opening page, to read while the rest is on its way. */
        for (int i = 0; i < s_turn.nmsgs && s_turn.texts; i++) {
            if (s_turn.msgs[i].len) {
                return muse_hatch_caption_at(msg_text(i), 0, out, cap);
            }
        }
        return false;
    }
    size_t len = strlen(text);
    uint32_t frames = m->pcm_frames ? m->pcm_frames : (uint32_t)(len * MIC_RATE / SPEECH_CHARS_PER_S);
    size_t at = frames ? (size_t)((uint64_t)(played - m->pcm_start) * len / frames) : 0;
    if (at >= len) {
        at = len ? len - 1 : 0;
    }
    return muse_hatch_caption_at(text, at, out, cap);
}

size_t muse_hatch_turn_read(int16_t *pcm, size_t frames, int wait_ms)
{
    return xStreamBufferReceive(s_out, pcm, frames * sizeof(int16_t), pdMS_TO_TICKS(wait_ms)) / sizeof(int16_t);
}

/* ---- MP3 self-test (bench 'm'): decodes the embedded test_reply.mp3 ---- */
/* Adapted from muse_chat_session.cpp; the reply decoder task 10 wires up works the same way. */

typedef struct {
    uint32_t step;   /* Q16 input samples per output sample */
    uint32_t pos;
    int16_t prev;
} resampler_t;

static void resampler_init(resampler_t *r, int in_rate, int out_rate)
{
    r->step = (uint32_t)(((uint64_t)in_rate << 16) / (uint64_t)out_rate);
    r->pos = 0;
    r->prev = 0;
}

/* Linear interpolation; state carries across calls. out must hold n*out/in + 2. */
static size_t resample(resampler_t *r, const int16_t *in, size_t n, int16_t *out)
{
    size_t o = 0;
    if (!n) {
        return 0;
    }
    while ((r->pos >> 16) < n) {
        size_t i = r->pos >> 16;
        int32_t a = i ? in[i - 1] : r->prev;
        int32_t b = in[i];
        out[o++] = (int16_t)(a + (((b - a) * (int32_t)(r->pos & 0xffff)) >> 16));
        r->pos += r->step;
    }
    r->pos -= (uint32_t)(n << 16);
    r->prev = in[n - 1];
    return o;
}

static size_t mp3_selftest(int16_t **pcm_out)
{
    extern const uint8_t mp3_start[] asm("_binary_test_reply_mp3_start");
    extern const uint8_t mp3_end[] asm("_binary_test_reply_mp3_end");
    size_t len = (size_t)(mp3_end - mp3_start);
    mp3dec_t *dec = psram_alloc(sizeof(mp3dec_t));
    int16_t *pcm = psram_alloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    size_t cap = MIC_RATE * 10;
    int16_t *out = psram_alloc(cap * sizeof(int16_t));
    if (!dec || !pcm || !out) {
        free(dec);
        free(pcm);
        free(out);
        return 0;
    }
    mp3dec_init(dec);
    resampler_t rs = { 0 };
    int rate = 0, frames = 0;
    size_t off = 0, n = 0;
    int64_t t0 = now_us();
    while (off < len) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(dec, mp3_start + off, (int)(len - off), pcm, &info);
        if (!info.frame_bytes) {
            break;
        }
        off += (size_t)info.frame_bytes;
        if (!samples) {
            continue;
        }
        frames++;
        if (info.channels == 2) {
            for (int k = 0; k < samples; k++) {
                pcm[k] = (int16_t)((pcm[2 * k] + pcm[2 * k + 1]) / 2);
            }
        }
        if (rate != info.hz) {
            rate = info.hz;
            resampler_init(&rs, info.hz, MIC_RATE);
        }
        if (n + (size_t)samples * MIC_RATE / (size_t)rate + 2 > cap) {
            break;
        }
        n += resample(&rs, pcm, (size_t)samples, out + n);
    }
    ESP_LOGI(TAG, "mp3 selftest: %u bytes, %d frames at %d Hz -> %u samples (%.2f s) in %lld ms", (unsigned)len, frames,
             rate, (unsigned)n, n / (double)MIC_RATE, (long long)((now_us() - t0) / 1000));
    free(dec);
    free(pcm);
    *pcm_out = out;
    return n;
}

typedef struct {
    TaskHandle_t caller;
    int16_t *pcm;
    size_t n;
} selftest_t;

static void selftest_body(void *arg)
{
    selftest_t *st = arg;
    st->n = mp3_selftest(&st->pcm);
    xTaskNotifyGive(st->caller);
    vTaskSuspend(NULL);   /* the caller deletes it, which frees the PSRAM stack */
}

/* minimp3 wants ~16 KB of stack, more than the voice task has. */
size_t muse_hatch_mp3_selftest(int16_t **pcm_out)
{
    selftest_t st = { xTaskGetCurrentTaskHandle(), NULL, 0 };
    TaskHandle_t task;
    if (xTaskCreatePinnedToCoreWithCaps(selftest_body, "mp3_selftest", 32 * 1024, &st, 5, &task, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        return 0;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDeleteWithCaps(task);
    *pcm_out = st.pcm;
    return st.n;
}
