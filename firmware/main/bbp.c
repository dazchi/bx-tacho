#include "bbp.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
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

static const char *TAG = "bbp";

#define PKT_LEN             17
#define CURVE_LEN           32
#define SET_LEN             12      // B0..B7, 70..73
#define NOTIFY_GAP_MS       10      // between the packets of a set
#define SUBSCRIBE_SETTLE_MS 100     // before the first status after subscribing
#define NOTIFY_RETRIES      20      // ...of NOTIFY_GAP_MS while the stack is out of buffers
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
    uint16_t curve[CURVE_LEN];
} bbp_event_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;    // guards s_store (written by the worker only)
static bbp_store_t s_store;
static uint8_t s_uid[6];

static volatile bool s_attached;
static volatile uint8_t s_batt_raw = 250;
static bool s_radio_on;
static volatile bool s_stopping;    // going to deep sleep: no more advertising

// Link state, written by the NimBLE host task
static volatile bool s_connected;
static volatile bool s_subscribed;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
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

static bool notify(const uint8_t pkt[PKT_LEN])
{
    for (int attempt = 0; attempt < NOTIFY_RETRIES; attempt++) {
        if (!s_connected || !s_subscribed) {
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
    ESP_LOGW(TAG, "notify gave up: no buffers");
    return false;
}

static void send_status(void)
{
    bbp_store_t st;
    uint8_t pkt[PKT_LEN];
    snapshot(&st);
    build_status(pkt, &st);
    notify(pkt);
}

static void send_set(void)
{
    bbp_store_t st;
    uint8_t pkts[SET_LEN][PKT_LEN];
    snapshot(&st);
    build_set(pkts, &st);
    for (int i = 0; i < SET_LEN; i++) {
        if (i > 0) {
            vTaskDelay(pdMS_TO_TICKS(NOTIFY_GAP_MS));
        }
        if (!notify(pkts[i])) {
            return;
        }
    }
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

static void worker_task(void *arg)
{
    bbp_event_t ev;
    for (;;) {
        xQueueReceive(s_queue, &ev, portMAX_DELAY);
        switch (ev.type) {
        case EV_ATTACH:
            send_status();
            break;
        case EV_SUBSCRIBED:
            vTaskDelay(pdMS_TO_TICKS(SUBSCRIBE_SETTLE_MS));
            send_status();
            break;
        case EV_SHOT:
            add_shot(&ev);
            send_set();       // the app first, then the slower flash write
            store_save();
            break;
        case EV_CMD:
            if (ev.cmd == 0x51) {
                send_status();
            } else if (ev.cmd == 0x74) {
                send_set();
            } else {
                // 0x75 (meaning unconfirmed: clear history or sensor test), 0x61, others
                ESP_LOGI(TAG, "command 0x%02x ignored", ev.cmd);
            }
            break;
        case EV_CLEAR:
            xSemaphoreTake(s_lock, portMAX_DELAY);
            memset(&s_store, 0, sizeof(s_store));
            xSemaphoreGive(s_lock);
            store_save();
            send_status();
            break;
        }
    }
}

static void post(const bbp_event_t *ev)
{
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
    if (OS_MBUF_PKTLEN(ctxt->om) < 1 || os_mbuf_copydata(ctxt->om, 0, 1, &ev.cmd) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    post(&ev);
    return 0;
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &CMD_UUID.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                // Writes accepted here too: some emulators put commands on this one
                .uuid = &DATA_UUID.u,
                .access_cb = chr_access,
                .val_handle = &s_data_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {0},
        },
    },
    {0},
};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_subscribed = false;
            s_connected = true;
            ESP_LOGI(TAG, "app connected");
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        s_subscribed = false;
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "app disconnected (reason 0x%x)", event->disconnect.reason);
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_data_handle) {
            bool was = s_subscribed;
            s_subscribed = event->subscribe.cur_notify;
            if (s_subscribed && !was) {
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
    // Name in the advertisement (what apps filter on), the 128-bit service
    // UUID in the scan response: both together exceed 31 bytes
    struct ble_hs_adv_fields adv = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .name = (uint8_t *)BBP_DEVICE_NAME,
        .name_len = sizeof(BBP_DEVICE_NAME) - 1,
        .name_is_complete = 1,
    };
    struct ble_hs_adv_fields rsp = {
        .uuids128 = &SVC_UUID,
        .num_uuids128 = 1,
        .uuids128_is_complete = 1,
    };
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
    ESP_RETURN_ON_FALSE(ble_gatts_count_cfg(s_services) == 0, ESP_FAIL, TAG, "gatt count");
    ESP_RETURN_ON_FALSE(ble_gatts_add_svcs(s_services) == 0, ESP_FAIL, TAG, "gatt add");
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
    store_load();
    ESP_LOGI(TAG, "history: %u shots, %lu total, max %u", s_store.count,
             (unsigned long)s_store.total, s_store.max_sp);
    ESP_RETURN_ON_FALSE(xTaskCreate(worker_task, "bbp", 3072, NULL, 4, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    if (radio) {
        ESP_RETURN_ON_ERROR(radio_start(), TAG, "ble");
        s_radio_on = true;
        ESP_LOGI(TAG, "BLE on, advertising as %s", BBP_DEVICE_NAME);
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
