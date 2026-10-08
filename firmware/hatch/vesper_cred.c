/*
 * The node credential in NVS (task 11). See vesper_cred.h.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned, written for this
 * project; not derived from the Meta muse-gadget-sdk sources.
 */
#include "vesper_cred.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "vesper_cred";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_cred[VC_CRED_MAX + 1];   /* internal RAM */

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
    set_ram(credential);
    esp_err_t err = run(credential);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "couldn't save the node credential: %s (kept until reboot)", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool vesper_cred_forget(void)
{
    set_ram(NULL);
    esp_err_t err = run(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "couldn't erase the node credential: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "node credential forgotten");
    }
    return err == ESP_OK;
}
