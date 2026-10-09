/*
 * Wake word (task 20): ESP-SR WakeNet on the idle mic feed. See vesper_wakenet.h.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources.
 */
#include "vesper_wakenet.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wn_iface.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "vesper_wake.h"

static const char *TAG = "vesper_wake";

#define NVS_NS "muse"
#define KEY_ON "wake_on"
#define KEY_THR "wake_thr"
#define SAMPLE_RATE 16000

/* The model packed into the image (components/muse/CMakeLists.txt, vesper_srmodels.S). */
extern const uint8_t vesper_srmodels_start[];
extern const uint8_t vesper_srmodels_end[];

typedef enum {
    ST_NONE = 0,   /* init not run */
    ST_NO_MODEL,   /* the packed model is missing or unreadable */
    ST_FAILED,     /* WakeNet wouldn't start (memory, wrong rate or channels) */
    ST_READY,
} wake_state_t;

static wake_state_t s_state;
static const esp_wn_iface_t *s_wn;
static model_iface_data_t *s_model;
static int16_t *s_block;            /* one WakeNet chunk, internal RAM */
static vw_reblock_t s_rb;
static vw_floor_t s_floor;
static int s_chunk;                  /* WakeNet's chunk size (samples) */

/* Settings: written by the console, applied by the voice task before its next detection. */
static atomic_bool s_on;
static atomic_int s_thr;             /* permille */
static atomic_int s_thr_applied;     /* what WakeNet has; -1: apply s_thr */

/* For >status: counts and timings only. */
static atomic_uint s_wakes, s_turns, s_no_speech, s_pressed, s_not_ready;
static atomic_uint s_detect_max_us, s_detect_avg_us;
static atomic_int s_floor_x10;
static atomic_uint s_stack_free;     /* the voice task's least free stack seen after a detection (bytes) */
static unsigned s_detects;
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

static bool load_model(const char **name)
{
    size_t size = (size_t)(vesper_srmodels_end - vesper_srmodels_start);
    if (size < 4 + 36) {
        return false;
    }
    srmodel_list_t *models = srmodel_load(vesper_srmodels_start);
    if (!models || models->num < 1) {
        return false;
    }
    *name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    return *name && !strcmp(*name, CONFIG_VESPER_WAKE_MODEL);
}

static unsigned detect_once(void)
{
    int64_t t0 = esp_timer_get_time();
    s_wn->detect(s_model, s_block);
    return (unsigned)(esp_timer_get_time() - t0);
}

bool vesper_wakenet_init(void)
{
    bool on = CONFIG_VESPER_WAKE_DEFAULT_ON;
    int thr = vw_clamp_threshold(CONFIG_VESPER_WAKE_THRESHOLD);
    nvs_read(&on, &thr);
    atomic_store(&s_on, on);
    atomic_store(&s_thr, thr);
    atomic_store(&s_thr_applied, -1);
    vw_floor_init(&s_floor);
    atomic_store(&s_floor_x10, (int)(s_floor.db * 10));

    size_t int0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t ps0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const char *name = NULL;
    if (!load_model(&name)) {
        s_state = ST_NO_MODEL;
        ESP_LOGE(TAG, "wake word off: no model %s in this image (push to talk is unaffected)",
                 CONFIG_VESPER_WAKE_MODEL);
        return false;
    }
    s_wn = esp_wn_handle_from_name(name);
    s_model = s_wn ? s_wn->create(name, DET_MODE_95) : NULL;
    if (!s_model) {
        s_state = ST_FAILED;
        ESP_LOGE(TAG, "wake word off: WakeNet %s didn't start (push to talk is unaffected)", name);
        return false;
    }
    s_chunk = s_wn->get_samp_chunksize(s_model);
    int rate = s_wn->get_samp_rate(s_model);
    int channels = s_wn->get_channel_num(s_model);
    if (s_chunk <= 0 || s_chunk > 4096 || rate != SAMPLE_RATE || channels != 1) {
        ESP_LOGE(TAG, "wake word off: WakeNet wants %d-sample chunks at %d Hz, %d channel(s); the mic is 16 kHz mono",
                 s_chunk, rate, channels);
        s_wn->destroy(s_model);
        s_model = NULL;
        s_state = ST_FAILED;
        return false;
    }
    s_block = heap_caps_aligned_calloc(16, s_chunk, sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_block) {
        ESP_LOGE(TAG, "wake word off: no memory for a %d-sample block", s_chunk);
        s_wn->destroy(s_model);
        s_model = NULL;
        s_state = ST_FAILED;
        return false;
    }
    vw_reblock_init(&s_rb, s_block, (size_t)s_chunk);
    s_wn->set_det_threshold(s_model, thr / 1000.0f, 1);
    atomic_store(&s_thr_applied, thr);
    /* Time a few detections on silence now, on this core, as a first CPU figure (the voice
     * task keeps an average and maximum for >status); then forget them. */
    unsigned worst = 0, total = 0;
    for (int i = 0; i < 8; i++) {
        unsigned us = detect_once();
        total += us;
        worst = us > worst ? us : worst;
    }
    s_wn->clean(s_model);
    s_state = ST_READY;
    size_t int1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t ps1 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "wake word %s: %s (\"%s\"), %d-sample chunks (%d ms), threshold %.3f (model default %.3f), "
                  "detect %u us avg / %u us max at boot",
             on ? "on" : "off (>wake=on)", name, s_wn->get_word_name(s_model, 1), s_chunk, s_chunk * 1000 / SAMPLE_RATE,
             thr / 1000.0, (double)s_wn->get_det_threshold(s_model, 1), total / 8, worst);
    ESP_LOGI(TAG, "wake word memory: model %u bytes in the image; WakeNet took %d B internal, %d B PSRAM; "
                  "free now %u B internal (largest %u), %u B PSRAM",
             (unsigned)(vesper_srmodels_end - vesper_srmodels_start), (int)(int0 - int1), (int)(ps0 - ps1),
             (unsigned)int1, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)ps1);
    return true;
}

bool vesper_wakenet_on(void)
{
    return s_state == ST_READY && atomic_load(&s_on);
}

void vesper_wakenet_reset(void)
{
    if (s_state != ST_READY) {
        return;
    }
    vw_reblock_clear(&s_rb);
    s_wn->clean(s_model);
}

bool vesper_wakenet_feed(const int16_t *pcm, size_t n, float chunk_db)
{
    vw_floor_update(&s_floor, chunk_db);
    atomic_store(&s_floor_x10, (int)(s_floor.db * 10));
    if (!vesper_wakenet_on()) {
        s_was_on = false;
        return false;
    }
    if (!s_was_on) {
        vesper_wakenet_reset();   /* (re)started: nothing from before counts */
        s_was_on = true;
    }
    int thr = atomic_load(&s_thr);
    if (thr != atomic_load(&s_thr_applied)) {
        s_wn->set_det_threshold(s_model, thr / 1000.0f, 1);
        atomic_store(&s_thr_applied, thr);
    }
    bool heard = false;
    size_t off = 0;
    while (off < n) {
        off += vw_reblock_push(&s_rb, pcm + off, n - off);
        if (!vw_reblock_full(&s_rb)) {
            break;
        }
        int64_t t0 = esp_timer_get_time();
        wakenet_state_t r = s_wn->detect(s_model, s_block);
        unsigned us = (unsigned)(esp_timer_get_time() - t0);
        vw_reblock_clear(&s_rb);
        unsigned avg = atomic_load(&s_detect_avg_us);
        atomic_store(&s_detect_avg_us, avg ? (avg * 63 + us) / 64 : us);
        if (us > atomic_load(&s_detect_max_us)) {
            atomic_store(&s_detect_max_us, us);
        }
        if (s_detects++ % 256 == 0) {
            atomic_store(&s_stack_free, (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        if (r == WAKENET_DETECTED) {
            heard = true;
            atomic_fetch_add(&s_wakes, 1);
            ESP_LOGI(TAG, "wake word heard (floor %.1f dBFS, detect %u us)", (double)s_floor.db, us);
            s_wn->clean(s_model);   /* one detection per utterance */
            vw_reblock_clear(&s_rb);
            break;
        }
    }
    return heard;
}

float vesper_wakenet_floor_db(void)
{
    return s_floor.db;
}

void vesper_wakenet_count(vesper_wake_outcome_t outcome)
{
    switch (outcome) {
    case VESPER_WAKE_TURN: atomic_fetch_add(&s_turns, 1); break;
    case VESPER_WAKE_NO_SPEECH: atomic_fetch_add(&s_no_speech, 1); break;
    case VESPER_WAKE_PRESSED: atomic_fetch_add(&s_pressed, 1); break;
    case VESPER_WAKE_NOT_READY: atomic_fetch_add(&s_not_ready, 1); break;
    }
}

bool vesper_wakenet_set_on(bool on)
{
    atomic_store(&s_on, on);
    return nvs_write(KEY_ON, true, on ? 1 : 0);
}

bool vesper_wakenet_set_threshold(int permille)
{
    int thr = vw_clamp_threshold(permille);
    atomic_store(&s_thr, thr);
    return nvs_write(KEY_THR, false, (unsigned)thr);
}

int vesper_wakenet_threshold(void)
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

int vesper_wakenet_status_json(char *out, size_t cap)
{
    /* Integers only: no floating-point printf on the serial task's small stack. */
    int thr = atomic_load(&s_thr);
    int fl = atomic_load(&s_floor_x10);
    int fl_abs = fl < 0 ? -fl : fl;
    return snprintf(out, cap,
                    "{\"state\":\"%s\",\"model\":\"%s\",\"threshold\":%d.%03d,\"chunk\":%d,\"wakes\":%u,\"turns\":%u,"
                    "\"no_speech\":%u,\"pressed\":%u,\"not_ready\":%u,\"detect_us\":{\"avg\":%u,\"max\":%u},"
                    "\"floor_db\":%s%d.%d,\"stack_free\":%u}",
                    state_name(), CONFIG_VESPER_WAKE_MODEL, thr / 1000, thr % 1000, s_chunk, atomic_load(&s_wakes),
                    atomic_load(&s_turns), atomic_load(&s_no_speech), atomic_load(&s_pressed),
                    atomic_load(&s_not_ready), atomic_load(&s_detect_avg_us), atomic_load(&s_detect_max_us),
                    fl < 0 ? "-" : "", fl_abs / 10, fl_abs % 10, atomic_load(&s_stack_free));
}
