/*
 * Wake word (task 22): "Hey Vesper" on the idle mic feed. See vesper_wakeword.h.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources.
 */
#include "vesper_wakeword.h"

#include <stdatomic.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "vesper_wake.h"
#include "vesper_wakeword_engine.h"

static const char *TAG = "vesper_wake";

#define NVS_NS "muse"
#define KEY_ON "wake_on"
#define KEY_THR "wake_hv_thr"
#define SAMPLE_RATE 16000
#define MODEL_NAME "hey_vesper"

/* The model linked into the image (components/muse/CMakeLists.txt, vesper_hv_model.S). */
extern const uint8_t vesper_hv_model_start[];
extern const uint8_t vesper_hv_model_end[];

typedef enum {
    ST_NONE = 0,   /* init not run */
    ST_NO_MODEL,   /* the linked model is missing or isn't one */
    ST_FAILED,     /* the engine wouldn't start (memory, the model's contract) */
    ST_READY,
} wake_state_t;

static wake_state_t s_state;
static uint8_t *s_arena;
static vw_floor_t s_floor;

/* Settings: written by the console, applied by the voice task before its next feed. */
static atomic_bool s_on;
static atomic_int s_thr;             /* permille */
static atomic_int s_thr_applied;     /* what the engine has */

/* For >status: counts and timings only. */
static atomic_uint s_wakes, s_turns, s_no_speech, s_pressed, s_not_ready;
static atomic_uint s_detect_max_us, s_detect_avg_us;
static atomic_int s_floor_x10;
static atomic_uint s_score_max;      /* permille: the highest window score since the last reset */
static atomic_uint s_stack_free;     /* the voice task's least free stack seen after an inference (bytes) */
static unsigned s_feeds;
static bool s_was_on;

static bool nvs_read(bool *on, int *thr)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t v8;
    uint16_t v16;
    if (nvs_get_u8(h, KEY_ON, &v8) == ESP_OK) {
        *on = v8 != 0;
    }
    if (nvs_get_u16(h, KEY_THR, &v16) == ESP_OK) {
        *thr = vw_clamp_threshold(v16);
    }
    nvs_close(h);
    return true;
}

static bool nvs_write(const char *key, bool is_u8, unsigned value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = is_u8 ? nvs_set_u8(h, key, (uint8_t)value) : nvs_set_u16(h, key, (uint16_t)value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

static unsigned score_permille(void)
{
    const vm_detect_t *d = vwe_detector();
    return d->w > 0 ? (unsigned)(d->peak_sum * 1000u / (255u * (unsigned)d->w)) : 0;
}

bool vesper_wakeword_init(void)
{
    bool on = CONFIG_VESPER_WAKE_DEFAULT_ON;
    int thr = vw_clamp_threshold(CONFIG_VESPER_WAKE_THRESHOLD);
    nvs_read(&on, &thr);
    atomic_store(&s_on, on);
    atomic_store(&s_thr, thr);
    atomic_store(&s_thr_applied, thr);
    vw_floor_init(&s_floor);
    atomic_store(&s_floor_x10, (int)(s_floor.db * 10));

    size_t model_len = (size_t)(vesper_hv_model_end - vesper_hv_model_start);
    if (model_len < 1024) {
        s_state = ST_NO_MODEL;
        ESP_LOGE(TAG, "wake word off: no model in this image (push to talk is unaffected)");
        return false;
    }
    size_t int0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t ps0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    /* The arena in PSRAM only, as ESPHome's micro_wake_word does: internal RAM is what Wi-Fi,
     * TLS, BLE and the display share, and 1.0.3 died of an internal-RAM wake engine (firmware/
     * README.md, "Memory budget and the 1.0.3 crash"). No PSRAM for it: no wake word, never a
     * 32 KB bite out of internal RAM. */
    s_arena = heap_caps_aligned_alloc(16, CONFIG_VESPER_WAKE_ARENA, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_arena) {
        s_state = ST_FAILED;
        ESP_LOGE(TAG, "wake word off: no PSRAM for a %d B tensor arena (push to talk is unaffected)",
                 CONFIG_VESPER_WAKE_ARENA);
        return false;
    }
    vwe_error_t err = vwe_init(vesper_hv_model_start, model_len, s_arena, CONFIG_VESPER_WAKE_ARENA,
                               CONFIG_VESPER_WAKE_WINDOW, thr);
    if (err) {
        heap_caps_free(s_arena);
        s_arena = NULL;
        s_state = ST_FAILED;
        ESP_LOGE(TAG, "wake word off: %s (push to talk is unaffected)", err);
        return false;
    }
    /* Time the engine on 0.5 s of silence now, on this core, as a first CPU figure (the voice
     * task keeps an average and maximum for >status); then forget it. */
    static const int16_t zeros[320];
    unsigned worst = 0, total = 0, n = 0;
    for (int i = 0; i < 25; i++) {
        unsigned ran = 0;
        int64_t t0 = esp_timer_get_time();
        vwe_feed(zeros, 320, &ran);
        unsigned us = (unsigned)(esp_timer_get_time() - t0);
        if (ran) {
            total += us;
            n++;
            worst = us > worst ? us : worst;
        }
    }
    vwe_reset();
    s_state = ST_READY;
    size_t int1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t ps1 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "wake word %s: \"Hey Vesper\" (microWakeWord, %u B model), threshold %d.%03d, window %d; "
                  "20 ms chunk with an inference: %u us avg / %u us max at boot (%u timed)",
             on ? "on" : "off (>wake=on)", (unsigned)model_len, thr / 1000, thr % 1000, CONFIG_VESPER_WAKE_WINDOW,
             n ? total / n : 0, worst, n);
    ESP_LOGI(TAG, "wake word memory: arena %u of %d B used (PSRAM); the engine took %d B internal, %d B PSRAM; "
                  "free now %u B internal (largest %u, lowest since boot %u), %u B PSRAM; "
                  "init stack %u B unused",
             (unsigned)vwe_arena_used(), CONFIG_VESPER_WAKE_ARENA, (int)(int0 - int1), (int)(ps0 - ps1),
             (unsigned)int1, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), (unsigned)ps1,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    return true;
}

bool vesper_wakeword_on(void)
{
    return s_state == ST_READY && atomic_load(&s_on);
}

void vesper_wakeword_reset(void)
{
    if (s_state != ST_READY) {
        return;
    }
    vwe_reset();
    atomic_store(&s_score_max, 0);
}

bool vesper_wakeword_feed(const int16_t *pcm, size_t n, float chunk_db)
{
    vw_floor_update(&s_floor, chunk_db);
    atomic_store(&s_floor_x10, (int)(s_floor.db * 10));
    if (!vesper_wakeword_on()) {
        s_was_on = false;
        return false;
    }
    if (!s_was_on) {
        vesper_wakeword_reset();   /* (re)started: nothing from before counts */
        s_was_on = true;
    }
    int thr = atomic_load(&s_thr);
    if (thr != atomic_load(&s_thr_applied)) {
        vwe_set_cutoff(thr);
        atomic_store(&s_thr_applied, thr);
    }
    unsigned ran = 0;
    int64_t t0 = esp_timer_get_time();
    bool heard = vwe_feed(pcm, n, &ran);
    unsigned us = (unsigned)(esp_timer_get_time() - t0);
    if (ran) {
        unsigned avg = atomic_load(&s_detect_avg_us);
        atomic_store(&s_detect_avg_us, avg ? (avg * 63 + us) / 64 : us);
        if (us > atomic_load(&s_detect_max_us)) {
            atomic_store(&s_detect_max_us, us);
        }
        atomic_store(&s_score_max, score_permille());
        if (s_feeds++ % 256 == 0) {
            atomic_store(&s_stack_free, (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
    }
    if (heard) {
        unsigned score = score_permille();
        atomic_fetch_add(&s_wakes, 1);
        ESP_LOGI(TAG, "wake word heard (score %u.%03u, floor %.1f dBFS, %u us)", score / 1000, score % 1000,
                 (double)s_floor.db, us);
    }
    return heard;
}

float vesper_wakeword_floor_db(void)
{
    return s_floor.db;
}

void vesper_wakeword_count(vesper_wake_outcome_t outcome)
{
    switch (outcome) {
    case VESPER_WAKE_TURN: atomic_fetch_add(&s_turns, 1); break;
    case VESPER_WAKE_NO_SPEECH: atomic_fetch_add(&s_no_speech, 1); break;
    case VESPER_WAKE_PRESSED: atomic_fetch_add(&s_pressed, 1); break;
    case VESPER_WAKE_NOT_READY: atomic_fetch_add(&s_not_ready, 1); break;
    }
}

bool vesper_wakeword_set_on(bool on)
{
    atomic_store(&s_on, on);
    return nvs_write(KEY_ON, true, on ? 1 : 0);
}

bool vesper_wakeword_set_threshold(int permille)
{
    int thr = vw_clamp_threshold(permille);
    atomic_store(&s_thr, thr);
    return nvs_write(KEY_THR, false, (unsigned)thr);
}

int vesper_wakeword_threshold(void)
{
    return atomic_load(&s_thr);
}

static const char *state_name(void)
{
    switch (s_state) {
    case ST_READY: return atomic_load(&s_on) ? "on" : "off";
    case ST_NO_MODEL: return "no_model";
    case ST_FAILED: return "failed";
    default: return "not_started";
    }
}

int vesper_wakeword_status_json(char *out, size_t cap)
{
    /* Integers only: no floating-point printf on the serial task's small stack. */
    int thr = atomic_load(&s_thr);
    int fl = atomic_load(&s_floor_x10);
    int fl_abs = fl < 0 ? -fl : fl;
    unsigned sc = atomic_load(&s_score_max);
    return snprintf(out, cap,
                    "{\"state\":\"%s\",\"model\":\"%s\",\"threshold\":%d.%03d,\"window\":%d,\"wakes\":%u,\"turns\":%u,"
                    "\"no_speech\":%u,\"pressed\":%u,\"not_ready\":%u,\"detect_us\":{\"avg\":%u,\"max\":%u},"
                    "\"score_max\":%u.%03u,\"arena\":%u,\"floor_db\":%s%d.%d,\"stack_free\":%u,"
                    "\"heap\":{\"int\":%u,\"int_min\":%u,\"int_largest\":%u,\"psram\":%u}}",
                    state_name(), MODEL_NAME, thr / 1000, thr % 1000, CONFIG_VESPER_WAKE_WINDOW, atomic_load(&s_wakes),
                    atomic_load(&s_turns), atomic_load(&s_no_speech), atomic_load(&s_pressed),
                    atomic_load(&s_not_ready), atomic_load(&s_detect_avg_us), atomic_load(&s_detect_max_us),
                    sc / 1000, sc % 1000, (unsigned)vwe_arena_used(), fl < 0 ? "-" : "", fl_abs / 10, fl_abs % 10,
                    atomic_load(&s_stack_free), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
