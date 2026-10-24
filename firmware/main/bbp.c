// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
#include "bbp.h"

#include <string.h>
#include "ble_console.h"
#include "ble_ota.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "settings.h"
#if BXT_HAVE_BBP_EXT
#include "bbp_ext.h"
#endif

static const char *TAG = "bbp";

#define PKT_LEN             17
#define CURVE_LEN           32
#define SET_LEN             12      // B0..B7, 70..73
#define NOTIFY_GAP_MS       10      // between the packets of a set
#define SUBSCRIBE_SETTLE_MS 100     // before the first status after subscribing
// While the stack is out of buffers, retry a packet this long (in NOTIFY_GAP_MS
// steps); then the link is dropped, so the app starts a clean session
#define NOTIFY_TIMEOUT_MS   1500
#define ADV_ITVL_MIN_MS     300
#define ADV_ITVL_MAX_MS     400

#define STATE_DETACHED      0x00
#define STATE_ATTACHED      0x04

#define NVS_NAMESPACE       "bbp"
#define NVS_KEY             "hist"
// Bump when bbp_store_t changes layout; a stale blob is then ignored.
#define STORE_VERSION       1

// 55c4xxxx-f8eb-11ec-b939-0242ac120002, little-endian byte order
#define BBP_UUID(b1, b0) BLE_UUID128_INIT(0x02, 0x00, 0x12, 0xac, 0x42, 0x02, 0x39, 0xb9, \
                                          0xec, 0x11, 0xeb, 0xf8, b0, b1, 0xc4, 0x55)
static const ble_uuid128_t SVC_UUID = BBP_UUID(0x00, 0x00);
static const ble_uuid128_t CMD_UUID = BBP_UUID(0xf0, 0x01);    // app -> device
static const ble_uuid128_t DATA_UUID = BBP_UUID(0xf0, 0x02);   // device -> app

typedef struct {
    uint32_t version;
    uint32_t total;               // shots ever
    uint16_t max_sp;              // highest shoot power ever
    uint16_t count;               // valid entries in sp[]
    uint16_t sp[BBP_HISTORY_LEN]; // oldest first
    uint16_t curve[CURVE_LEN];    // latest shot, revolution periods in 8 us units
} bbp_store_t;

typedef enum {
    EV_ATTACH,        // send a status
    EV_SUBSCRIBED,    // app started listening: status after a short settle
    EV_SHOT,          // add to the history, save, send the set
    EV_CMD,           // command byte written by the app
    EV_CLEAR,         // console: wipe the history
} bbp_event_type_t;

typedef struct {
    uint8_t type;
    uint8_t cmd;
    uint16_t sp;
    uint32_t gen;             // link session it belongs to (s_gen when posted)
    uint16_t curve[CURVE_LEN];
} bbp_event_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;    // guards s_store (written by the worker only)
static bbp_store_t s_store;
static uint8_t s_uid[6];

static volatile bool s_attached;
static volatile uint8_t s_batt_raw = 250;
static bool s_radio_on;
static bool s_ext;                  // optional extension in use (bbp_ext.h), from boot
static int s_ext_next_ms = -1;      // when it wants to be polled again (worker)
static volatile bool s_stopping;    // going to deep sleep: no more advertising

// Link state, written by the NimBLE host task. s_gen changes with every connect,
// disconnect and subscription change, so nothing queued or being sent for one
// session reaches the next: events and packets carry the generation they were
// meant for.
static volatile bool s_connected;
static volatile bool s_subscribed;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile uint32_t s_gen = 1;
static volatile int64_t s_sub_us;   // when the app subscribed

static uint16_t s_data_handle;
static uint8_t s_own_addr_type;

static void advertise(void);

// ---------------------------------------------------------------- storage

static void store_load(void)
{
    memset(&s_store, 0, sizeof(s_store));
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    bbp_store_t blob;
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(h, NVS_KEY, &blob, &len);
    nvs_close(h);
    if (err == ESP_OK && len == sizeof(blob) && blob.version == STORE_VERSION &&
        blob.count <= BBP_HISTORY_LEN) {
        s_store = blob;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "saved history unreadable or outdated, starting empty");
    }
}

static void store_save(void)
{
    bbp_store_t blob;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    blob = s_store;
    xSemaphoreGive(s_lock);
    blob.version = STORE_VERSION;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, &blob, sizeof(blob));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "saving history failed: %s", esp_err_to_name(err));
    }
}

// ---------------------------------------------------------------- packets

static void put_u16(uint8_t *p, uint32_t v)
{
    if (v > 0xFFFF) {
        v = 0xFFFF;
    }
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void snapshot(bbp_store_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_store;
    xSemaphoreGive(s_lock);
}

static void build_status(uint8_t pkt[PKT_LEN], const bbp_store_t *st)
{
    memset(pkt, 0, PKT_LEN);
    pkt[0] = 0xA0;
    pkt[3] = s_attached ? STATE_ATTACHED : STATE_DETACHED;
    pkt[4] = s_batt_raw;
    put_u16(&pkt[7], st->max_sp);
    put_u16(&pkt[9], st->total);
    memcpy(&pkt[11], s_uid, sizeof(s_uid));
#if BXT_HAVE_BBP_EXT
    if (s_ext) {
        bbp_ext_status(pkt, s_gen);
    }
#endif
}

// Entry i of a 16-bit table spread over packets of 8 entries each
static uint8_t *slot(uint8_t (*pkts)[PKT_LEN], int i)
{
    return &pkts[i / 8][1 + (i % 8) * 2];
}

static void build_set(uint8_t pkts[SET_LEN][PKT_LEN], const bbp_store_t *st)
{
    memset(pkts, 0, SET_LEN * PKT_LEN);
    for (int i = 0; i < 8; i++) {
        pkts[i][0] = 0xB0 + i;
    }
    for (int i = 0; i < 4; i++) {
        pkts[8 + i][0] = 0x70 + i;
    }

    // History: oldest first, so the latest shot is in slot count-1. Slots 48-49
    // land in B6 bytes 1-4, ahead of its tail fields.
    for (int i = 0; i < st->count; i++) {
        put_u16(slot(pkts, i), st->sp[i]);
    }
    put_u16(&pkts[6][7], st->max_sp);
    put_u16(&pkts[6][9], st->total);
    pkts[6][11] = (uint8_t)st->count;

    uint8_t sum = 0;
    for (int p = 0; p < 7; p++) {
        for (int b = 1; b < PKT_LEN; b++) {
            sum += pkts[p][b];
        }
    }
    pkts[7][16] = sum;

    for (int i = 0; i < CURVE_LEN; i++) {
        put_u16(slot(&pkts[8], i), st->curve[i]);
    }
}

// ---------------------------------------------------------------- sending

// Sends one packet to the session `gen`; false if that session is gone
static bool notify(const uint8_t pkt[PKT_LEN], uint32_t gen)
{
    for (int attempt = 0; attempt < NOTIFY_TIMEOUT_MS / NOTIFY_GAP_MS; attempt++) {
        if (!s_connected || !s_subscribed || s_gen != gen) {
            return false;
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(pkt, PKT_LEN);
        if (om != NULL) {
            int rc = ble_gatts_notify_custom(s_conn, s_data_handle, om);   // consumes om
            if (rc == 0) {
                return true;
            }
            if (rc != BLE_HS_ENOMEM) {
                ESP_LOGW(TAG, "notify failed: %d", rc);
                return false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(NOTIFY_GAP_MS));   // out of buffers: let the stack catch up
    }
    ESP_LOGW(TAG, "notify gave up after %d ms without buffers: dropping the link", NOTIFY_TIMEOUT_MS);
    ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    return false;
}

static void send_status(uint32_t gen, const char *reason)
{
    bbp_store_t st;
    uint8_t pkt[PKT_LEN];
    snapshot(&st);
    build_status(pkt, &st);
    if (notify(pkt, gen)) {
        ESP_LOGI(TAG, "TX A0 state=0x%02x battery=%u max=%u total=%lu reason=%s", pkt[3], pkt[4],
                 st.max_sp, (unsigned long)st.total, reason);
    }
}

// The 12 packets from one snapshot, never interleaved with anything else (the
// worker sends one thing at a time)
static void send_set(uint32_t gen, const char *source)
{
    bbp_store_t st;
    uint8_t pkts[SET_LEN][PKT_LEN];
    snapshot(&st);
    build_set(pkts, &st);
    int points = 0;
    while (points < CURVE_LEN && st.curve[points] != 0) {
        points++;
    }
    ESP_LOGI(TAG, "dump start source=%s count=%u total=%lu max=%u curve_points=%d", source,
             st.count, (unsigned long)st.total, st.max_sp, points);
    for (int i = 0; i < SET_LEN; i++) {
        if (i > 0) {
            vTaskDelay(pdMS_TO_TICKS(NOTIFY_GAP_MS));
        }
        if (!notify(pkts[i], gen)) {
            ESP_LOGW(TAG, "dump stopped at page %02X: the session ended", pkts[i][0]);
            return;
        }
        ESP_LOGD(TAG, "TX page=%02X index=%d", pkts[i][0], i);
    }
    ESP_LOGI(TAG, "dump complete");
}

// ---------------------------------------------------------------- worker

static void add_shot(const bbp_event_t *ev)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_store.count == BBP_HISTORY_LEN) {
        memmove(&s_store.sp[0], &s_store.sp[1], (BBP_HISTORY_LEN - 1) * sizeof(s_store.sp[0]));
        s_store.count--;
    }
    s_store.sp[s_store.count++] = ev->sp;
    s_store.total++;
    if (ev->sp > s_store.max_sp) {
        s_store.max_sp = ev->sp;
    }
    memcpy(s_store.curve, ev->curve, sizeof(s_store.curve));
    xSemaphoreGive(s_lock);
}

// The optional extension (bbp_ext.h) gets a look after every event, and again
// when it asked to be polled; it may have a status packet sent
static void ext_poll(void)
{
#if BXT_HAVE_BBP_EXT
    if (!s_ext) {
        return;
    }
    uint32_t gen = s_gen;
    bbp_ext_action_t action = bbp_ext_poll(gen, s_subscribed, s_attached, esp_timer_get_time());
    s_ext_next_ms = action.next_ms;
    if (action.send_status && s_subscribed) {
        send_status(gen, action.reason ? action.reason : "EXT");
    }
#endif
}

static TickType_t ext_wait(void)
{
    return s_ext_next_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(s_ext_next_ms);
}

static void handle_command(const bbp_event_t *ev)
{
    switch (ev->cmd) {
    case 0x51:
        ESP_LOGI(TAG, "CMD 51 STATUS");
        send_status(ev->gen, "CMD_51");
        break;
    case 0x74:
        ESP_LOGI(TAG, "CMD 74 DUMP");
        send_set(ev->gen, "CMD_74");
        break;
    case 0x75:
        // "Clear history" in one emulator, "sensor test" in another: not
        // confirmed, so the history isn't wiped on it
        ESP_LOGI(TAG, "CMD 75 CLEAR? ignored (meaning unconfirmed)");
        break;
    case 0x61:
        ESP_LOGI(TAG, "CMD 61 (meaning unknown) ignored");
        break;
    default:
        ESP_LOGI(TAG, "CMD UNKNOWN %02x ignored", ev->cmd);
        break;
    }
}

static void worker_task(void *arg)
{
    bbp_event_t ev;
    for (;;) {
        if (xQueueReceive(s_queue, &ev, ext_wait()) != pdTRUE) {
            ext_poll();
            continue;
        }
        // Link events from a session that has ended since are dropped
        bool stale = ev.gen != s_gen;
        switch (ev.type) {
        case EV_ATTACH:
            if (!stale) {
                send_status(ev.gen, "LOAD_CHANGE");
            }
            break;
        case EV_SUBSCRIBED:
            if (stale) {
                break;
            }
            ESP_LOGI(TAG, "session start gen=%lu", (unsigned long)ev.gen);
            vTaskDelay(pdMS_TO_TICKS(SUBSCRIBE_SETTLE_MS));
            send_status(ev.gen, "SUBSCRIBE_INITIAL");
#if BXT_HAVE_BBP_EXT
            if (s_ext) {
                bbp_ext_session_start(ev.gen, s_sub_us);
            }
#endif
            break;
        case EV_SHOT:
            add_shot(&ev);
            if (s_subscribed) {
                send_set(s_gen, "AUTO");   // the app first, then the slower flash write
            }
            store_save();
            break;
        case EV_CMD:
            if (stale) {
                ESP_LOGI(TAG, "CMD %02x from an ended session (gen %lu) dropped", ev.cmd,
                         (unsigned long)ev.gen);
            } else {
                handle_command(&ev);
            }
            break;
        case EV_CLEAR:
            xSemaphoreTake(s_lock, portMAX_DELAY);
            memset(&s_store, 0, sizeof(s_store));
            xSemaphoreGive(s_lock);
            store_save();
            if (s_subscribed) {
                send_status(s_gen, "CLEAR");
            }
            break;
        }
        ext_poll();
    }
}

static void post(bbp_event_t *ev)
{
    ev->gen = s_gen;
    if (s_queue != NULL && xQueueSend(s_queue, ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, event %d dropped", ev->type);
    }
}

// ---------------------------------------------------------------- BLE

static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    // Commands are one byte; take the first byte of anything longer
    bbp_event_t ev = {.type = EV_CMD};
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 1 || os_mbuf_copydata(ctxt->om, 0, 1, &ev.cmd) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    ESP_LOGI(TAG, "RX gen=%lu handle=%u chr=%s len=%u data=%02x%s", (unsigned long)s_gen,
             conn_handle, attr_handle == s_data_handle ? "f002" : "f001", len, ev.cmd,
             len > 1 ? "..." : "");
    post(&ev);
    return 0;
}

static struct ble_gatt_chr_def s_chrs[] = {
    {
        .uuid = &CMD_UUID.u,
        .access_cb = chr_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        // Writes accepted here too: some emulators put commands on this one
        // (an extension may change the flags, radio_start)
        .uuid = &DATA_UUID.u,
        .access_cb = chr_access,
        .val_handle = &s_data_handle,
        .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {0},
};
#define DATA_CHR (s_chrs[1])

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = s_chrs,
    },
    {0},
};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    ble_console_gap_event(event);
    ble_ota_gap_event(event);
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_subscribed = false;
            s_connected = true;
            s_gen++;
            ESP_LOGI(TAG, "connect handle=%u gen=%lu", event->connect.conn_handle, (unsigned long)s_gen);
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        s_subscribed = false;
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_gen++;
        ESP_LOGI(TAG, "disconnect handle=%u reason=0x%x gen=%lu", event->disconnect.conn.conn_handle,
                 event->disconnect.reason, (unsigned long)s_gen);
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_data_handle) {
            bool was = s_subscribed;
            s_subscribed = event->subscribe.cur_notify;
            if (s_subscribed != was) {
                s_gen++;
                ESP_LOGI(TAG, "subscribe %s handle=%u gen=%lu", s_subscribed ? "on" : "off",
                         event->subscribe.conn_handle, (unsigned long)s_gen);
            }
            if (s_subscribed && !was) {
                s_sub_us = esp_timer_get_time();
                bbp_event_t ev = {.type = EV_SUBSCRIBED};
                post(&ev);
            }
        }
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    if (s_stopping) {
        return;
    }
    // Name and 128-bit service UUID together exceed the 31 bytes of an
    // advertisement, so one goes in the scan response: the name stays in the
    // advertisement (what apps filter on) unless an extension wants the UUID there
    bool uuid_first = false;
#if BXT_HAVE_BBP_EXT
    uuid_first = s_ext && bbp_ext_uuid_in_advertisement();
#endif
    struct ble_hs_adv_fields name = {
        .name = (uint8_t *)BBP_DEVICE_NAME,
        .name_len = sizeof(BBP_DEVICE_NAME) - 1,
        .name_is_complete = 1,
    };
    struct ble_hs_adv_fields uuid = {
        .uuids128 = &SVC_UUID,
        .num_uuids128 = 1,
        .uuids128_is_complete = 1,
    };
    struct ble_hs_adv_fields adv = uuid_first ? uuid : name;
    struct ble_hs_adv_fields rsp = uuid_first ? name : uuid;
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = BLE_GAP_ADV_ITVL_MS(ADV_ITVL_MIN_MS),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(ADV_ITVL_MAX_MS),
    };
    int rc = ble_gap_adv_set_fields(&adv);
    if (rc == 0) {
        rc = ble_gap_adv_rsp_set_fields(&rsp);
    }
    if (rc == 0) {
        rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    }
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "advertising failed: %d", rc);
    } else if (rc == 0) {
        ESP_LOGI(TAG, "advertising%s", uuid_first ? " (service UUID first)" : "");
    }
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "no BLE address");
        return;
    }
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset, reason %d", reason);
}

static void host_task(void *arg)
{
    nimble_port_run();   // returns only after nimble_port_stop()
    nimble_port_freertos_deinit();
}

static esp_err_t radio_start(void)
{
    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble init");
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
#if BXT_HAVE_BBP_EXT
    if (s_ext) {
        DATA_CHR.flags = bbp_ext_data_flags(DATA_CHR.flags);
    }
#endif
    ESP_RETURN_ON_FALSE(ble_gatts_count_cfg(s_services) == 0, ESP_FAIL, TAG, "gatt count");
    ESP_RETURN_ON_FALSE(ble_gatts_add_svcs(s_services) == 0, ESP_FAIL, TAG, "gatt add");
    ESP_RETURN_ON_FALSE(ble_console_init() == 0, ESP_FAIL, TAG, "console");
    ESP_RETURN_ON_FALSE(ble_ota_init() == 0, ESP_FAIL, TAG, "ota");
    ESP_RETURN_ON_FALSE(ble_svc_gap_device_name_set(BBP_DEVICE_NAME) == 0, ESP_FAIL, TAG, "name");

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

// ---------------------------------------------------------------- API

esp_err_t bbp_init(bool radio)
{
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(8, sizeof(bbp_event_t));
    ESP_RETURN_ON_FALSE(s_lock && s_queue, ESP_ERR_NO_MEM, TAG, "alloc");
    esp_read_mac(s_uid, ESP_MAC_BT);
#if BXT_HAVE_BBP_EXT
    if (settings_bbp_ext()) {
        s_ext = bbp_ext_api_version() == BBP_EXT_API_VERSION;
        if (!s_ext) {
            ESP_LOGE(TAG, "extension %s is for interface version %d, not %d: not used",
                     bbp_ext_name(), bbp_ext_api_version(), BBP_EXT_API_VERSION);
        }
    }
#endif
    store_load();
    ESP_LOGI(TAG, "history: %u shots, %lu total, max %u", s_store.count,
             (unsigned long)s_store.total, s_store.max_sp);
    ESP_RETURN_ON_FALSE(xTaskCreate(worker_task, "bbp", 3072, NULL, 4, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    if (radio) {
        ESP_RETURN_ON_ERROR(radio_start(), TAG, "ble");
        s_radio_on = true;
        ESP_LOGI(TAG, "BLE on, advertising as %s%s%s", BBP_DEVICE_NAME,
                 s_ext ? ", extension " : "", s_ext ? bbp_ext_built_in() : "");
    }
    return ESP_OK;
}

void bbp_set_attached(bool attached)
{
    if (attached == s_attached) {
        return;
    }
    s_attached = attached;
    if (s_subscribed) {
        bbp_event_t ev = {.type = EV_ATTACH};
        post(&ev);
    }
}

void bbp_set_battery_percent(int percent)
{
    percent = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    s_batt_raw = (uint8_t)((percent * 250 + 50) / 100);
}

void bbp_record_shot(int sp_rpm, const uint32_t *periods_us, int n)
{
    if (sp_rpm <= 0) {
        return;
    }
    bbp_event_t ev = {.type = EV_SHOT, .sp = sp_rpm > 0xFFFF ? 0xFFFF : (uint16_t)sp_rpm};
    for (int i = 0; i < n && i < CURVE_LEN; i++) {
        uint32_t ticks = (periods_us[i] + 4) / 8;   // 8 us units, rounded
        ev.curve[i] = ticks == 0 ? 1 : (ticks > 0xFFFF ? 0xFFFF : (uint16_t)ticks);
    }
    post(&ev);
}

void bbp_get_info(bbp_info_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->count = s_store.count;
    out->total = s_store.total;
    out->max_sp = s_store.max_sp;
    memcpy(out->sp, s_store.sp, sizeof(out->sp));
    xSemaphoreGive(s_lock);
    out->radio_on = s_radio_on;
    out->connected = s_connected;
    out->subscribed = s_subscribed;
}

const char *bbp_ext_built_in(void)
{
#if BXT_HAVE_BBP_EXT
    return bbp_ext_name();
#else
    return NULL;
#endif
}

bool bbp_ext_active(void)
{
    return s_ext;
}

bbp_link_t bbp_link_state(void)
{
    if (!s_radio_on) {
        return BBP_LINK_OFF;
    }
    return s_connected ? BBP_LINK_CONNECTED : BBP_LINK_ADVERTISING;
}

void bbp_clear_history(void)
{
    bbp_event_t ev = {.type = EV_CLEAR};
    post(&ev);
}

void bbp_stop(void)
{
    if (!s_radio_on) {
        return;
    }
    s_stopping = true;
    ble_gap_adv_stop();
    if (s_connected) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        for (int i = 0; s_connected && i < 20; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
