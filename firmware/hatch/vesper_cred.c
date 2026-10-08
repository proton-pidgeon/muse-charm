/*
 * The node credential in NVS (task 11). See vesper_cred.h.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources.
 */
#include "vesper_cred.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "vesper_cred";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_cred[VC_CRED_MAX + 1];   /* internal RAM */

/*
 * Store and forget run one at a time (s_mutex, held across the NVS write),
 * and a forget always wins over a store it overlaps.
 *
 * The rule: a forget bumps s_forgets BEFORE it waits for the mutex; a store
 * samples s_forgets on entry, BEFORE it takes the mutex, and after its own
 * write it compares. Any change means a forget was requested after the store
 * began, so the store erases its own write, clears RAM and returns false.
 *
 * Interleavings considered (S = store on the hatch task, F = forget on the
 * reset task or the hatch task; "gen" = s_forgets):
 *  1. F completes, then S starts: S samples the bumped gen, nothing changes
 *     during it, S keeps its credential. Correct: the forget came first.
 *  2. S samples gen, F bumps gen and waits, S writes, sees the change, erases
 *     and returns false; F then erases again (harmless). Nothing left.
 *  3. F bumps gen, S samples AFTER the bump but before F gets the mutex, then
 *     S takes the mutex first: S keeps its write, F then erases it. Nothing
 *     left either, and S returned true. Its caller must therefore not trust
 *     the return alone. claim_step compares vesper_cred_generation() from
 *     before the store and checks vesper_cred_present() afterwards, then
 *     re-checks after publishing "claimed". It also reconciles on its next
 *     pass: claimed but no credential in RAM means claim again, through the
 *     normal start/backoff path.
 *  4. S takes the mutex before F bumps: same as 2.
 *  5. F (forget-now from the setup reset) waits for the mutex while S is in
 *     run(): no deadlock, because the NVS writer task never takes the mutex,
 *     so S finishes and releases it.
 *  6. Two forgets: serialised by the mutex; the second erase finds nothing.
 */
static StaticSemaphore_t s_mutex_buf;
static SemaphoreHandle_t s_mutex;
static atomic_uint s_forgets;

/* Created by vesper_cred_load() at boot, before any store or forget can run (the hatch
 * task, the console and the setup reset all start later); static storage, so it can't fail. */
static SemaphoreHandle_t mutex(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buf);
    }
    return s_mutex;
}

static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--) {
        *v++ = 0;
    }
}

static void set_ram(const char *value)
{
    portENTER_CRITICAL(&s_lock);
    wipe(s_cred, sizeof(s_cred));
    if (value) {
        strlcpy(s_cred, value, sizeof(s_cred));
    }
    portEXIT_CRITICAL(&s_lock);
}

void vesper_cred_load(void)
{
    mutex();   /* created here, before anything can store or forget */
    char buf[VC_CRED_MAX + 2];
    size_t n = sizeof(buf);
    nvs_handle_t h;
    esp_err_t err = nvs_open(VESPER_CRED_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_str(h, VESPER_CRED_KEY, buf, &n);
        nvs_close(h);
    }
    bool ok = err == ESP_OK && vc_valid_credential(buf);
    set_ram(ok ? buf : NULL);
    wipe(buf, sizeof(buf));
    if (err == ESP_OK && !ok) {
        ESP_LOGW(TAG, "the stored node credential is malformed; the node will claim again");
    } else {
        ESP_LOGI(TAG, "node credential: %s", ok ? "stored" : "none yet (the node will claim)");
    }
}

bool vesper_cred_present(void)
{
    portENTER_CRITICAL(&s_lock);
    bool have = s_cred[0] != '\0';
    portEXIT_CRITICAL(&s_lock);
    return have;
}

bool vesper_cred_get(char out[VC_CRED_MAX + 1])
{
    portENTER_CRITICAL(&s_lock);
    memcpy(out, s_cred, VC_CRED_MAX + 1);
    portEXIT_CRITICAL(&s_lock);
    return out[0] != '\0';
}

/* ---- NVS writes, on an internal-RAM stack ---- */

typedef struct {
    TaskHandle_t caller;
    const char *value;   /* NULL: erase */
    esp_err_t err;
} job_t;

static void writer(void *arg)
{
    job_t *j = arg;
    /* The caller's copy may be on a PSRAM stack: write from this internal one. */
    char value[VC_CRED_MAX + 1] = "";
    if (j->value) {
        strlcpy(value, j->value, sizeof(value));
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(VESPER_CRED_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = j->value ? nvs_set_str(h, VESPER_CRED_KEY, value) : nvs_erase_key(h, VESPER_CRED_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND && !j->value) {
            err = ESP_OK;   /* nothing to forget */
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    wipe(value, sizeof(value));
    j->err = err;
    xTaskNotifyGive(j->caller);
    vTaskDelete(NULL);
}

static esp_err_t run(const char *value)
{
    job_t j = { xTaskGetCurrentTaskHandle(), value, ESP_FAIL };
    /* xTaskCreate puts the stack in internal RAM. */
    if (xTaskCreate(writer, "vesper_cred", 3072, &j, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* the writer always notifies: j outlives it */
    return j.err;
}

bool vesper_cred_store(const char *credential)
{
    if (!vc_valid_credential(credential)) {
        return false;
    }
    unsigned gen = atomic_load(&s_forgets);   /* before the mutex: a forget requested from here on wins */
    xSemaphoreTake(mutex(), portMAX_DELAY);
    if (atomic_load(&s_forgets) != gen) {
        xSemaphoreGive(mutex());   /* a forget overtook us while we waited: write nothing */
        ESP_LOGW(TAG, "node credential forgotten before it was saved; not kept");
        return false;
    }
    set_ram(credential);
    esp_err_t err = run(credential);
    bool forgotten = atomic_load(&s_forgets) != gen;
    if (forgotten) {
        /* A forget (setup reset, >claim.forget) came in while this was being written: it wins. */
        run(NULL);
        set_ram(NULL);
        ESP_LOGW(TAG, "node credential forgotten while it was being saved; not kept");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "couldn't save the node credential: %s (kept until reboot)", esp_err_to_name(err));
    }
    xSemaphoreGive(mutex());
    return err == ESP_OK && !forgotten;
}

unsigned vesper_cred_generation(void)
{
    return atomic_load(&s_forgets);
}

bool vesper_cred_forget(void)
{
    atomic_fetch_add(&s_forgets, 1);   /* before the wait: a store in progress undoes itself */
    xSemaphoreTake(mutex(), portMAX_DELAY);
    set_ram(NULL);
    esp_err_t err = run(NULL);
    xSemaphoreGive(mutex());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "couldn't erase the node credential: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "node credential forgotten");
    }
    return err == ESP_OK;
}
