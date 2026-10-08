/*
 * The node's BLE host (task 11): Muse's phone-setup service (muse_ble.c,
 * unchanged) plus the Vesper claim service. See vesper_ble.h.
 *
 * Part of muse-charm (Vesper node firmware). Vesper-owned. The host start
 * (nimble_port_init, the services, the security config, the host task) and
 * the advertising follow the stock main/ble_server.c (Meta Platforms,
 * Apache-2.0), without Link's pairing service, its Noise records or the
 * factory-test service.
 */
#include "vesper_ble.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "muse_ble.h"
#include "muse_link.h"

static const char *TAG = "vesper_ble";

/* 76657370-6572-4e6f-6465-00000000000X ("vesperNode"), little-endian. */
#define VESPER_UUID(x) BLE_UUID128_INIT(x, 0, 0, 0, 0, 0, 0x65, 0x64, 0x6f, 0x4e, 0x72, 0x65, 0x70, 0x73, 0x65, 0x76)
static const ble_uuid128_t CLAIM_SVC_UUID = VESPER_UUID(0x01);
static const ble_uuid128_t CLAIM_CHR_UUID = VESPER_UUID(0x02);

#define CLAIM_JSON_MAX 96

static volatile bool s_started, s_synced, s_adv_active;
static volatile bool s_want_setup, s_want_claim;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static char s_name[32];
static uint16_t s_claim_handle;
static struct ble_npl_event s_update_ev;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_claim_json[CLAIM_JSON_MAX] = "{\"state\":\"starting\"}";

/* ---- the claim characteristic ---- */

static int claim_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    char json[CLAIM_JSON_MAX];
    portENTER_CRITICAL(&s_lock);
    memcpy(json, s_claim_json, sizeof(json));
    portEXIT_CRITICAL(&s_lock);
    return os_mbuf_append(ctxt->om, json, strlen(json)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def CLAIM_SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &CLAIM_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &CLAIM_CHR_UUID.u,
                .access_cb = claim_access,
                .val_handle = &s_claim_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ---- advertising (on the host task) ---- */

static int gap_event(struct ble_gap_event *ev, void *arg);

static void start_advertising(void)
{
    /* Advertising: flags + the complete name. Scan response: the claim service. */
    struct ble_hs_adv_fields adv = { 0 };
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.name = (uint8_t *)s_name;
    adv.name_len = strlen(s_name);
    adv.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields rc=%d", rc);
        return;
    }
    struct ble_hs_adv_fields rsp = { 0 };
    rsp.uuids128 = (ble_uuid128_t *)&CLAIM_SVC_UUID;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_rsp_set_fields rc=%d", rc);
    }
    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
        return;
    }
    s_adv_active = true;
    ESP_LOGI(TAG, "advertising as %s (%s)", s_name, s_want_claim ? "claim in progress" : "phone setup on");
}

/* Starts or stops advertising to match what's wanted. */
static void update_advertising(void)
{
    if (!s_synced) {
        return;
    }
    bool want = (s_want_setup || s_want_claim) && s_conn == BLE_HS_CONN_HANDLE_NONE;
    if (want && !s_adv_active) {
        start_advertising();
    } else if (!want && s_adv_active && s_conn == BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_adv_stop();
        s_adv_active = false;
        ESP_LOGI(TAG, "advertising off");
    }
}

/* Host-task work posted from other tasks: advertising and the claim notification. */
static void on_update(struct ble_npl_event *ev)
{
    (void)ev;
    update_advertising();
    if (s_claim_handle) {
        ble_gatts_chr_updated(s_claim_handle);   /* notifies a subscribed phone */
    }
}

static void post_update(void)
{
    if (s_started) {
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_update_ev);   /* no-op if already queued */
    }
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    /* Muse's setup service sees every event first: its passkey and link security. */
    int rc = muse_ble_gap_event(ev);
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_adv_active = false;
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            ESP_LOGI(TAG, "connected");
        } else {
            update_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_adv_active = false;
        ESP_LOGI(TAG, "disconnected (reason %d)", ev->disconnect.reason);
        update_advertising();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_adv_active = false;
        update_advertising();
        break;
    default:
        break;
    }
    return rc;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr rc=%d", rc);
        return;
    }
    s_synced = true;
    update_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset (reason %d)", reason);
    s_synced = false;
    s_adv_active = false;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* On the keeper task (internal stack; NimBLE reads its bonds from NVS here). */
static void start_host(const char *name)
{
    strlcpy(s_name, name && name[0] ? name : "MuseGadget", sizeof(s_name));
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s; no BLE", esp_err_to_name(err));
        return;
    }
    ble_svc_gap_init();
    ble_svc_gatt_init();
    const struct ble_gatt_svc_def *setup = muse_ble_services();
    int rc = ble_gatts_count_cfg(setup);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(setup);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "setup service rc=%d", rc);
    }
    rc = ble_gatts_count_cfg(CLAIM_SERVICES);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(CLAIM_SERVICES);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "claim service rc=%d", rc);
    }
    muse_ble_configure_host();   /* passkey pairing with bonding, as stock */
    ble_svc_gap_device_name_set(s_name);
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_npl_event_init(&s_update_ev, on_update, NULL);
    nimble_port_freertos_init(host_task);
    s_started = true;
    ESP_LOGI(TAG, "BLE host started");
}

/* ---- API ---- */

void vesper_ble_apply(const char *name, bool setup_on)
{
    s_want_setup = setup_on;
    if (!s_started && (setup_on || s_want_claim)) {
        start_host(name);
    }
    post_update();
}

void vesper_ble_set_claim(const char *state, const char *code, const char *room, bool advertise)
{
    char json[CLAIM_JSON_MAX];
    if (code && code[0]) {
        snprintf(json, sizeof(json), "{\"state\":\"%s\",\"code\":\"%s\"}", state, code);
    } else if (room && room[0]) {
        snprintf(json, sizeof(json), "{\"state\":\"%s\",\"room\":\"%s\"}", state, room);
    } else {
        snprintf(json, sizeof(json), "{\"state\":\"%s\"}", state);
    }
    bool changed;
    portENTER_CRITICAL(&s_lock);
    changed = strcmp(json, s_claim_json) != 0;
    memcpy(s_claim_json, json, sizeof(s_claim_json));
    portEXIT_CRITICAL(&s_lock);
    bool start = advertise != s_want_claim;
    s_want_claim = advertise;
    if (!s_started) {
        if (advertise && start) {
            muse_link_ble_apply();   /* the keeper starts the host (vesper_ble_apply), on its own stack */
        }
        return;
    }
    if (changed || start) {
        post_update();
    }
}

bool vesper_ble_started(void)
{
    return s_started;
}
