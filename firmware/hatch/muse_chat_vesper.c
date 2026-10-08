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
 *      message_done -> the message is queued to be said, with its speech URL
 *      if it has one (the TTS slot), error -> ERROR, done -> end of stream.
 *   4. Speech (task 10, the stock start_tts() slot filled): each finished
 *      message in turn is said. With an audio_url, the MP3 is fetched with a
 *      second GET (same bearer, same server), decoded by minimp3 on this task
 *      as it arrives, resampled to 16 kHz (vesper_audio.c) and queued for the
 *      voice task, which plays it (muse_hatch_turn_read) while its caption
 *      follows the speech (muse_hatch_turn_caption, timed by the audio). With
 *      no audio_url, or if the fetch or decode fails, the caption is paced
 *      over silence (16 chars/s) as in stock firmware; a fetch cut short
 *      plays what came and paces the rest of the caption over silence. The
 *      turn ends (DONE) once every message has been said or shown.
 *
 * Everything network-side runs on one task. The voice task talks to it through
 * a command queue, a stream buffer of mic audio, an event queue (captions) and
 * a stream buffer of reply audio, exactly as with the stock backend. A
 * generation number tags each turn so that events, and reply audio, of a
 * cancelled turn are dropped. No Meta account, token or transport is involved.
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
#include "vesper_audio.h"
#include "vesper_proto.h"

static const char *TAG = "vesper_chat";

#define MIC_RATE 16000
#define IN_BYTES (MIC_RATE * 2 * 8)        /* 8 s of mic backlog while connecting */
#define OUT_BYTES (MIC_RATE * 2 * 2)       /* 2 s of decoded reply audio */
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

/* Speech (task 10) */
#define TTS_BUF (192 * 1024)               /* MP3 fetched, not yet decoded: ~48 s at 32 kbps (PSRAM) */
#define TTS_READ 4096                      /* bytes per socket read */
#define TTS_CONNECT_TIMEOUT_MS 5000        /* the MP3 is ready by the time message_done names it */
#define TTS_POLL_MS 20                     /* MP3 read poll, between decodes */
#define TTS_IDLE_US (10 * 1000000LL)       /* no MP3 bytes this long: say what came, pace the rest */
#define TTS_MAX_BYTES (2 * 1024 * 1024)    /* a message's MP3, at most (the backend caps the text at 500 chars) */
/* Resampled samples one MP3 frame can make: 1152 mono samples, at worst 8 kHz -> 16 kHz, plus the flush. */
#define TTS_OUT_MAX (2 * (MINIMP3_MAX_SAMPLES_PER_FRAME / 2) + 2 * VA_RS_TAPS + 8)

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
    uint32_t pcm_start;      /* where its speech (or silence) starts in the reply audio */
    uint32_t pcm_frames;     /* how long it is; 0 until known (captions then guess 14 chars/s) */
    char audio[VP_URL_MAX];  /* its MP3 (message_done.audio_url, resolved), or "" */
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
    int tts_msg;             /* message being said (or shown), or -1 */
    bool silent;             /* tts_msg is paced by silence, not its MP3 */
    uint32_t pcm_out;        /* reply audio frames handed to the voice task */
    char reply_shown[EV_TEXT];
    int64_t t_status, t_text, t_done;
} turn_t;

/* ~23 KB, most of it the SSE parser: PSRAM on the AIPI, like the stock turn state. */
EXT_RAM_BSS_ATTR static turn_t s_turn;
static uint8_t s_rx[1024];

/* The message being said: its MP3 GET, the bytes not yet decoded, the decoder. */
typedef struct {
    esp_http_client_handle_t http;   /* open while the MP3 downloads */
    char content_type[48];
    int64_t content_length;  /* -1: not given (chunked) */
    uint64_t received;
    bool cut_short;          /* the download ended early: what came is said, the rest paced */
    bool flushed;            /* the resampler's tail is out */
    va_mp3buf_t mp3;
    mp3dec_t dec;
    va_resampler_t rs;
    int rate, channels, kbps;
    uint32_t frames;         /* MP3 frames decoded */
    int64_t t_open, t_headers, t_fetched, t_first, last_rx_us;
} tts_state_t;

/* ~20 KB (the decoder and the resampler's table): PSRAM, like the stock decoder state. */
EXT_RAM_BSS_ATTR static tts_state_t s_tts;
static uint8_t *s_tts_buf;   /* TTS_BUF, PSRAM, allocated once */
static int16_t *s_tts_pcm;   /* MINIMP3_MAX_SAMPLES_PER_FRAME: one decoded frame */
static int16_t *s_tts_out;   /* TTS_OUT_MAX: the frame at 16 kHz */

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

static void tts_stop(void);

static void turn_finish(void)
{
    http_close();
    tts_stop();   /* cancel, failure or time cap mid-speech: the MP3 GET goes too */
    s_turn.phase = P_IDLE;
    s_turn.tts_msg = -1;
    s_turn.silent = false;
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

/*
 * The TTS slot (task 10): a finished message with speech keeps its MP3's URL
 * (resolved by vp_resolve_audio_url, so on the configured server) until its
 * turn to be said; start_showing() fetches it then. No I/O here: this runs
 * inside the SSE parser. False leaves the message to be paced over silence.
 */
static bool vesper_tts_slot_offer(const char *abs_url, int msg)
{
    msg_t *m = &s_turn.msgs[msg];
    if (!s_tts_buf || !abs_url || strlcpy(m->audio, abs_url, sizeof(m->audio)) >= sizeof(m->audio)) {
        m->audio[0] = '\0';
        return false;
    }
    return true;
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
    bool speech = m->len && audio_url && vesper_tts_slot_offer(audio_url, i);
    ESP_LOGI(TAG, "message %d done (%u chars, %s)", i, (unsigned)m->len,
             speech ? "with speech" : audio_url ? "speech refused" : "no speech");
    /* said (or shown) in order, after the messages before it */
    m->tts = m->len ? TTS_QUEUED : TTS_FINISHED;
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
    s_turn.silent = false;
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

/* Hands reply audio to the voice task; a cancelled turn's never gets there. */
static void push_reply(const int16_t *pcm, size_t n)
{
    if (n && s_turn.gen == atomic_load(&s_gen)) {
        size_t bytes = n * sizeof(int16_t);
        size_t sent = xStreamBufferSend(s_out, pcm, bytes, 0);
        if (sent != bytes) {
            ESP_LOGW(TAG, "reply buffer full: %u of %u bytes dropped", (unsigned)(bytes - sent), (unsigned)bytes);
        }
    }
    s_turn.pcm_out += (uint32_t)n;
}

/* The caption is paced over silence from here: stock behaviour, or what's left after a failed fetch. */
static void go_silent(msg_t *m, const char *why)
{
    uint32_t said = s_turn.pcm_out - m->pcm_start;
    if (!said) {
        m->pcm_frames = va_silent_frames(m->len, MIC_RATE, TEXT_CHARS_PER_S);
    } else if (m->pcm_frames < said) {
        m->pcm_frames = said;
    }
    s_turn.silent = true;
    ESP_LOGI(TAG, "message %d: %s; caption paced over silence (%.2f s left)", s_turn.tts_msg, why,
             (m->pcm_frames - said) / (double)MIC_RATE + TEXT_HOLD_S);
}

static esp_err_t on_tts_http_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value &&
        !strcasecmp(e->header_key, "Content-Type")) {
        strlcpy(s_tts.content_type, e->header_value, sizeof(s_tts.content_type));
    }
    return ESP_OK;
}

/* Closes the MP3 GET and empties the MP3 buffer. Every way a message's speech ends comes here. */
static void tts_stop(void)
{
    if (s_tts.http) {
        esp_http_client_close(s_tts.http);
        esp_http_client_cleanup(s_tts.http);
        s_tts.http = NULL;
    }
    va_mp3_init(&s_tts.mp3, s_tts_buf, TTS_BUF);
}

/*
 * GET <the message's MP3> with the turn's bearer. Blocks for the connect and
 * the response headers only (the MP3 is ready: message_done came after it);
 * the body is read in short polls by tts_fetch(). False (nothing left open) if
 * it can't be said: the caller paces the caption silently instead.
 */
static bool tts_open(int i)
{
    msg_t *m = &s_turn.msgs[i];
    tts_stop();
    if (!s_tts_buf || !m->audio[0]) {
        return false;
    }
    s_tts.content_type[0] = '\0';
    s_tts.content_length = -1;
    s_tts.received = 0;
    s_tts.cut_short = s_tts.flushed = false;
    s_tts.rate = s_tts.channels = s_tts.kbps = 0;
    s_tts.frames = 0;
    s_tts.t_open = now_us();
    s_tts.t_headers = s_tts.t_fetched = s_tts.t_first = 0;
    esp_http_client_config_t cfg = {
        .url = m->audio,
        .method = HTTP_METHOD_GET,
        .timeout_ms = TTS_CONNECT_TIMEOUT_MS,
        .event_handler = on_tts_http_event,
        .buffer_size = 2048,
        .buffer_size_tx = VP_AUTH_MAX + 512,   /* the request line and headers, bearer included */
        .disable_auto_redirect = true,         /* never carry the bearer elsewhere */
        .crt_bundle_attach = s_turn.base.https ? esp_crt_bundle_attach : NULL,
    };
    s_tts.http = esp_http_client_init(&cfg);
    if (!s_tts.http) {
        ESP_LOGW(TAG, "tts: no HTTP client");
        return false;
    }
    esp_http_client_set_header(s_tts.http, s_turn.hdrs[0].name, s_turn.hdrs[0].value);   /* Authorization */
    esp_http_client_set_header(s_tts.http, s_turn.hdrs[1].name, s_turn.hdrs[1].value);   /* X-Node-Id */
    esp_http_client_set_header(s_tts.http, "Accept", "audio/mpeg");
    esp_err_t err = esp_http_client_open(s_tts.http, 0);
    int64_t cl = err == ESP_OK ? esp_http_client_fetch_headers(s_tts.http) : -1;
    int status = cl >= 0 ? esp_http_client_get_status_code(s_tts.http) : 0;
    bool chunked = cl >= 0 && esp_http_client_is_chunked_response(s_tts.http);
    bool type_ok = va_tts_content_type_ok(s_tts.content_type);
    s_tts.t_headers = now_us();
    /* never the URL: its id is a capability */
    ESP_LOGI(TAG, "tts: message %d: GET HTTP %d, Content-Length %lld%s, %s, headers in %d ms", i, status,
             (long long)(cl > 0 && !chunked ? cl : -1), chunked ? " (chunked)" : "",
             type_ok ? "audio/mpeg" : "not audio/mpeg", (int)((s_tts.t_headers - s_tts.t_open) / 1000));
    if (err != ESP_OK || status != 200 || !type_ok || cl > TTS_MAX_BYTES) {
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "tts: connect failed: %s", esp_err_to_name(err));
        }
        tts_stop();
        return false;
    }
    s_tts.content_length = cl > 0 && !chunked ? cl : -1;
    esp_http_client_set_timeout_ms(s_tts.http, TTS_POLL_MS);
    mp3dec_init(&s_tts.dec);
    s_tts.last_rx_us = now_us();
    return true;
}

/* The download is over: all of it, or cut short (what came is still said). */
static void tts_fetched(bool cut_short)
{
    if (s_tts.http) {
        esp_http_client_close(s_tts.http);
        esp_http_client_cleanup(s_tts.http);
        s_tts.http = NULL;
    }
    s_tts.mp3.ended = true;
    s_tts.cut_short = cut_short;
    s_tts.t_fetched = now_us();
    ESP_LOGI(TAG, "tts: fetched %llu of %lld bytes in %d ms%s", (unsigned long long)s_tts.received,
             (long long)s_tts.content_length, (int)((s_tts.t_fetched - s_tts.t_open) / 1000),
             cut_short ? " (cut short)" : "");
}

/* Reads MP3 while the buffer has room: flow control, so a long reply never overflows it. */
static void tts_fetch(void)
{
    for (int reads = 0; reads < 4 && s_tts.http; reads++) {
        size_t room = va_mp3_room(&s_tts.mp3);
        if (!room) {
            return;   /* the decoder makes room as the speaker plays */
        }
        int want = room < TTS_READ ? (int)room : TTS_READ;
        int n = esp_http_client_read(s_tts.http, (char *)s_tts.mp3.buf + s_tts.mp3.len, want);
        if (n > 0) {
            s_tts.mp3.len += (size_t)n;   /* n <= want <= room */
            s_tts.received += (uint64_t)n;
            s_tts.last_rx_us = now_us();
            if (s_tts.content_length > 0 && s_tts.received >= (uint64_t)s_tts.content_length) {
                tts_fetched(false);
            } else if (s_tts.received >= TTS_MAX_BYTES) {
                ESP_LOGW(TAG, "tts: MP3 over %d bytes, saying the start", TTS_MAX_BYTES);
                tts_fetched(true);
            }
            continue;
        }
        if (n == -ESP_ERR_HTTP_EAGAIN) {
            if (now_us() - s_tts.last_rx_us > TTS_IDLE_US) {
                ESP_LOGW(TAG, "tts: download stalled");
                tts_fetched(true);
            }
            return;
        }
        /* 0: the body ended; < 0: the connection dropped */
        bool complete = n == 0 && (s_tts.content_length > 0 ? s_tts.received >= (uint64_t)s_tts.content_length
                                                            : esp_http_client_is_complete_data_received(s_tts.http));
        tts_fetched(!complete);
        return;
    }
}

/*
 * Decodes buffered MP3 while the reply buffer has room, keeps the message's
 * length (for its caption) up to date, and finishes it once it's all out.
 * The stock decode() in muse_chat_session.cpp, with the anti-aliased
 * resampler (vesper_audio.c) in place of its linear one.
 */
static void tts_decode(void)
{
    int i = s_turn.tts_msg;
    msg_t *m = &s_turn.msgs[i];
    size_t off = 0;
    while (va_mp3_may_decode(&s_tts.mp3, off) && xStreamBufferSpacesAvailable(s_out) >= TTS_OUT_MAX * sizeof(int16_t)) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&s_tts.dec, s_tts.mp3.buf + off, (int)(s_tts.mp3.len - off), s_tts_pcm, &info);
        if (!info.frame_bytes) {
            if (s_tts.mp3.ended) {
                off = s_tts.mp3.len;   /* trailing junk */
            }
            break;
        }
        off += (size_t)info.frame_bytes;
        if (!samples) {
            continue;   /* an ID3 tag, or a frame skipped while syncing */
        }
        if (info.channels == 2) {
            for (int k = 0; k < samples; k++) {
                s_tts_pcm[k] = (int16_t)((s_tts_pcm[2 * k] + s_tts_pcm[2 * k + 1]) / 2);
            }
        }
        if (s_tts.rate != info.hz) {
            int64_t t0 = now_us();
            if (s_tts.rs.in_rate == (uint32_t)info.hz && s_tts.rs.out_rate == MIC_RATE) {
                va_rs_reset(&s_tts.rs);   /* the same rates as the last message: keep the table */
            } else if (!va_rs_init(&s_tts.rs, (uint32_t)info.hz, MIC_RATE)) {
                ESP_LOGW(TAG, "tts: can't resample %d Hz", info.hz);
                continue;
            }
            s_tts.rate = info.hz;
            s_tts.channels = info.channels;
            ESP_LOGI(TAG, "tts: reply audio %d Hz, %d ch, %d kbps -> %d Hz (resampler ready in %d ms)", info.hz,
                     info.channels, info.bitrate_kbps, MIC_RATE, (int)((now_us() - t0) / 1000));
        }
        size_t n = va_rs_process(&s_tts.rs, s_tts_pcm, (size_t)samples, s_tts_out);
        if (n && !s_tts.t_first) {
            s_tts.t_first = now_us();
        }
        push_reply(s_tts_out, n);
        s_tts.kbps = info.bitrate_kbps;
        s_tts.frames++;
    }
    va_mp3_consume(&s_tts.mp3, off);

    uint32_t said = s_turn.pcm_out - m->pcm_start;
    if (s_tts.kbps > 0) {
        /* what's out, plus what the bitrate says the rest (buffered and still to come) holds */
        uint64_t left = s_tts.mp3.len;
        if (!s_tts.mp3.ended && s_tts.content_length > 0 && s_tts.received < (uint64_t)s_tts.content_length) {
            left += (uint64_t)s_tts.content_length - s_tts.received;
        }
        m->pcm_frames = va_speech_frames(said, left, s_tts.kbps, MIC_RATE);
    }
    if (!va_mp3_drained(&s_tts.mp3)) {
        return;
    }
    if (s_tts.rate && !s_tts.flushed) {
        if (xStreamBufferSpacesAvailable(s_out) < TTS_OUT_MAX * sizeof(int16_t)) {
            return;   /* the resampler's tail goes next time */
        }
        push_reply(s_tts_out, va_rs_flush(&s_tts.rs, s_tts_out));
        s_tts.flushed = true;
        said = s_turn.pcm_out - m->pcm_start;
    }
    if (!said) {
        go_silent(m, "no audio decoded");
        return;
    }
    ESP_LOGI(TAG, "tts: message %d: %u MP3 frames at %d Hz -> %u samples at %d Hz (%.2f s of speech); "
             "first audio %d ms after the GET",
             i, (unsigned)s_tts.frames, s_tts.rate, (unsigned)said, MIC_RATE, said / (double)MIC_RATE,
             s_tts.t_first ? (int)((s_tts.t_first - s_tts.t_open) / 1000) : -1);
    if (s_tts.cut_short) {
        go_silent(m, "speech cut short");   /* the rest of the caption, at the speech's pace */
        return;
    }
    m->pcm_frames = said;
    m->tts = TTS_FINISHED;
    s_turn.tts_msg = -1;
}

/* Picks the next finished message to say (or show). The stock start_tts() slot, filled. */
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
        m->pcm_frames = 0;
        m->tts = TTS_ACTIVE;
        s_turn.tts_msg = i;
        s_turn.silent = false;
        show_reply_start(i);
        if (!m->audio[0]) {
            go_silent(m, "no speech");
        } else if (!tts_open(i)) {
            go_silent(m, "speech unavailable");
        }
        return;
    }
}

/* Queues the shown message's silence while the reply buffer has room. From muse_chat_session.cpp. */
static void pace_silently(void)
{
    static const int16_t zeros[256];
    msg_t *m = &s_turn.msgs[s_turn.tts_msg];
    uint32_t end = m->pcm_start + m->pcm_frames + TEXT_HOLD_S * MIC_RATE;
    while (s_turn.pcm_out < end && xStreamBufferSpacesAvailable(s_out) >= sizeof(zeros)) {
        uint32_t n = end - s_turn.pcm_out < 256 ? end - s_turn.pcm_out : 256;
        push_reply(zeros, n);
    }
    if (s_turn.pcm_out >= end) {
        m->tts = TTS_FINISHED;
        s_turn.tts_msg = -1;
        s_turn.silent = false;
    }
}

/* The message being said: its MP3 in and decoded, or its silence. */
static void speak(void)
{
    if (s_turn.tts_msg < 0) {
        return;
    }
    if (s_turn.silent) {
        pace_silently();
        return;
    }
    tts_fetch();
    tts_decode();
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
            speak();
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
    /* Speech: allocated once and kept, so no path of a turn has anything to free. Without
     * them replies are still shown, paced over silence (vesper_tts_slot_offer declines). */
    s_tts_buf = psram_alloc(TTS_BUF);
    s_tts_pcm = psram_alloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    s_tts_out = psram_alloc(TTS_OUT_MAX * sizeof(int16_t));
    if (!s_tts_buf || !s_tts_pcm || !s_tts_out) {
        ESP_LOGE(TAG, "no memory for speech; replies will be captions only");
        free(s_tts_buf);
        free(s_tts_pcm);
        free(s_tts_out);
        s_tts_buf = NULL;
        s_tts_pcm = s_tts_out = NULL;
    }
    va_mp3_init(&s_tts.mp3, s_tts_buf, TTS_BUF);
    if (!s_node_id[0]) {
        ESP_LOGW(TAG, "no node id set; turns will be refused");
    }
    /* Stack in PSRAM, like the stock session task: TLS and the MP3 decoder (~16 KB of scratch) run here. */
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

/*
 * Caption-sync evidence for the serial log (voice task only): one line per
 * page turn, with where the speech is and where the caption is, never the
 * text. In sync, char/len tracks audio/speech.
 */
static void log_caption_page(int msg, size_t played, uint32_t start, uint32_t frames, size_t at, size_t len,
                             const char *page)
{
    static int s_msg = -1, s_page;
    static size_t s_played;
    static uint32_t s_hash;
    uint32_t h = 2166136261u;   /* FNV-1a of the page, to notice it turn */
    for (const char *c = page; *c; c++) {
        h = (h ^ (uint8_t)*c) * 16777619u;
    }
    if (played < s_played) {
        s_msg = -1;   /* a new turn's reply */
    }
    s_played = played;
    if (msg == s_msg && h == s_hash) {
        return;
    }
    s_page = msg == s_msg ? s_page + 1 : 1;
    s_msg = msg;
    s_hash = h;
    ESP_LOGI(TAG, "caption: message %d page %d at %.2f s of %.2f s (char %u of %u)", msg, s_page,
             (played - start) / (double)MIC_RATE, frames / (double)MIC_RATE, (unsigned)at, (unsigned)len);
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
    bool ok = muse_hatch_caption_at(text, at, out, cap);
    if (ok) {
        log_caption_page((int)(m - s_turn.msgs), played, m->pcm_start, frames, at, len, out);
    }
    return ok;
}

size_t muse_hatch_turn_read(int16_t *pcm, size_t frames, int wait_ms)
{
    return xStreamBufferReceive(s_out, pcm, frames * sizeof(int16_t), pdMS_TO_TICKS(wait_ms)) / sizeof(int16_t);
}

/* ---- MP3 self-test (bench 'm'): decodes the embedded test_reply.mp3 ---- */
/* Adapted from muse_chat_session.cpp; decodes and resamples as a spoken reply is (tts_decode). */

static size_t mp3_selftest(int16_t **pcm_out)
{
    extern const uint8_t mp3_start[] asm("_binary_test_reply_mp3_start");
    extern const uint8_t mp3_end[] asm("_binary_test_reply_mp3_end");
    size_t len = (size_t)(mp3_end - mp3_start);
    mp3dec_t *dec = psram_alloc(sizeof(mp3dec_t));
    va_resampler_t *rs = psram_alloc(sizeof(va_resampler_t));
    int16_t *pcm = psram_alloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    size_t cap = MIC_RATE * 10;
    int16_t *out = psram_alloc(cap * sizeof(int16_t));
    if (!dec || !rs || !pcm || !out) {
        free(dec);
        free(rs);
        free(pcm);
        free(out);
        return 0;
    }
    mp3dec_init(dec);
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
            if (!va_rs_init(rs, (uint32_t)info.hz, MIC_RATE)) {
                break;
            }
            rate = info.hz;
        }
        if (n + va_rs_max_out(rs, (size_t)samples) + va_rs_max_out(rs, 0) > cap) {
            break;
        }
        n += va_rs_process(rs, pcm, (size_t)samples, out + n);
    }
    if (rate) {
        n += va_rs_flush(rs, out + n);   /* room was kept for it */
    }
    ESP_LOGI(TAG, "mp3 selftest: %u bytes, %d frames at %d Hz -> %u samples (%.2f s) in %lld ms", (unsigned)len, frames,
             rate, (unsigned)n, n / (double)MIC_RATE, (long long)((now_us() - t0) / 1000));
    free(dec);
    free(rs);
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
