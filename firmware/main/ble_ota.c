// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
#include "ble_ota.h"

#include <string.h>
#include "app.h"
#include "battery.h"
#include "bbp.h"
#include "ble_console.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/task.h"
#include "host/ble_hs.h"

static const char *TAG = "bleota";

#define MSG_MAX           520       // one write: op + up to an MTU's worth of data
#define MSG_BUF_LEN       (BLE_OTA_WINDOW + 4096)   // the window, plus per-message overhead
#define MIN_BATT_PCT      50        // on battery only, more than this: an update takes about a minute
#define RESTART_DELAY_MS  1000      // after END's reply, before the restart

#define OP_BEGIN  0x01
#define OP_DATA   0x02
#define OP_END    0x03
#define OP_ABORT  0x04

// fcf6xxxx-7902-4817-adcc-c0380ba2609d, little-endian byte order (as ble_console.c)
#define OTA_UUID(b1, b0) BLE_UUID128_INIT(0x9d, 0x60, 0xa2, 0x0b, 0x38, 0xc0, 0xcc, 0xad, \
                                          0x17, 0x48, 0x02, 0x79, b0, b1, 0xf6, 0xfc)
static const ble_uuid128_t SVC_UUID = OTA_UUID(0x00, 0x11);
static const ble_uuid128_t CHR_UUID = OTA_UUID(0x00, 0x12);

static MessageBufferHandle_t s_msgs;   // host task -> OTA task, one message per write
static esp_pm_lock_handle_t s_pm_lock;
static uint16_t s_chr_handle;

// Link state, written by the NimBLE host task
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_lost;           // a write didn't fit in s_msgs

// The update, owned by the OTA task (read by the display)
static esp_ota_handle_t s_handle;
static const esp_partition_t *s_part;
static volatile bool s_active;
static volatile bool s_restarting;
static volatile uint32_t s_size;
static volatile uint32_t s_written;
static uint32_t s_acked;

// ---------------------------------------------------------------- replies

static void reply(uint8_t op, ble_ota_status_t status, uint32_t value)
{
    uint8_t pkt[6] = {op, (uint8_t)status, (uint8_t)value, (uint8_t)(value >> 8),
                      (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(pkt, sizeof(pkt));
    for (int attempt = 0; om != NULL && attempt < 100; attempt++) {
        int rc = ble_gatts_notify_custom(conn, s_chr_handle, om);   // consumes om
        if (rc != BLE_HS_ENOMEM) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));   // out of buffers: let the stack catch up
        om = ble_hs_mbuf_from_flat(pkt, sizeof(pkt));
    }
}

// ---------------------------------------------------------------- the update

static void finish(void)
{
    if (s_active) {
        s_active = false;
        esp_pm_lock_release(s_pm_lock);
    }
}

static void fail(uint8_t op, ble_ota_status_t status)
{
    if (s_active) {
        esp_ota_abort(s_handle);
        ESP_LOGW(TAG, "update stopped at %lu of %lu bytes (op %u, status %d)",
                 (unsigned long)s_written, (unsigned long)s_size, op, status);
    }
    finish();
    reply(op, status, 0);
}

static void begin(const uint8_t *msg, size_t len)
{
    if (s_active) {
        esp_ota_abort(s_handle);   // a new update replaces an unfinished one
        finish();
    }
    uint32_t size = len >= 5 ? msg[1] | msg[2] << 8 | msg[3] << 16 | (uint32_t)msg[4] << 24 : 0;
    app_snapshot_t snap;
    app_get_snapshot(&snap);

    ble_ota_status_t status = BLE_OTA_OK;
    s_part = esp_ota_get_next_update_partition(NULL);
    if (!ble_console_unlocked()) {
        status = BLE_OTA_LOCKED;
    } else if (!battery_usb_present() && (!snap.batt.vbat_valid || snap.batt.percent <= MIN_BATT_PCT)) {
        // With USB power the battery doesn't matter (and isn't measured while charging)
        status = BLE_OTA_BATTERY;
    } else if (s_part == NULL) {
        status = BLE_OTA_NO_SLOT;
    } else if (size == 0 || size > s_part->size) {
        status = BLE_OTA_TOO_BIG;
    }
    if (status != BLE_OTA_OK) {
        reply(OP_BEGIN, status, 0);
        return;
    }

    esp_pm_lock_acquire(s_pm_lock);   // full speed, and the radio never waits for a wake-up
    s_active = true;
    s_size = size;
    s_written = 0;
    s_acked = 0;
    s_lost = false;
    app_note_activity();
    ESP_LOGI(TAG, "update: %lu bytes to %s", (unsigned long)size, s_part->label);
    // Erases room for the whole image now, so writing doesn't stop for it later
    if (esp_ota_begin(s_part, size, &s_handle) != ESP_OK) {
        finish();   // nothing to abort
        reply(OP_BEGIN, BLE_OTA_FLASH, 0);
        return;
    }
    reply(OP_BEGIN, BLE_OTA_OK, ble_att_mtu(s_conn));
}

static void data(const uint8_t *msg, size_t len)
{
    if (!s_active) {
        return;   // after a failure: the client has been told, drop the rest
    }
    size_t n = len - 1;
    if (s_lost || s_written + n > s_size) {
        fail(OP_DATA, BLE_OTA_SEQUENCE);
        return;
    }
    if (esp_ota_write(s_handle, &msg[1], n) != ESP_OK) {
        fail(OP_DATA, BLE_OTA_BAD_IMAGE);   // the first bytes aren't an app image, or flash
        return;
    }
    s_written += n;
    if (s_written - s_acked >= BLE_OTA_ACK_BYTES || s_written == s_size) {
        s_acked = s_written;
        reply(OP_DATA, BLE_OTA_OK, s_written);
        app_note_activity();   // keeps BX Tacho awake and the display on
    }
}

static void end(void)
{
    if (!s_active || s_lost || s_written != s_size) {
        fail(OP_END, BLE_OTA_SEQUENCE);
        return;
    }
    esp_err_t err = esp_ota_end(s_handle);   // checks the image, its hash and the chip; frees the handle
    esp_app_desc_t desc;
    ble_ota_status_t status = BLE_OTA_OK;
    if (err != ESP_OK) {
        status = err == ESP_ERR_OTA_VALIDATE_FAILED ? BLE_OTA_BAD_IMAGE : BLE_OTA_FLASH;
    } else if (esp_ota_get_partition_description(s_part, &desc) != ESP_OK ||
               strcmp(desc.project_name, esp_app_get_description()->project_name) != 0) {
        status = BLE_OTA_NOT_OURS;
    } else if (esp_ota_set_boot_partition(s_part) != ESP_OK) {
        status = BLE_OTA_FLASH;
    }
    if (status != BLE_OTA_OK) {
        finish();
        ESP_LOGW(TAG, "update rejected (status %d)", status);
        reply(OP_END, status, 0);
        return;
    }

    ESP_LOGI(TAG, "update done: v%s in %s, restarting", desc.version, s_part->label);
    s_restarting = true;
    reply(OP_END, BLE_OTA_OK, s_written);
    vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
    bbp_stop();   // the app sees the disconnect at once
    esp_restart();
}

static void ota_task(void *arg)
{
    static uint8_t msg[MSG_MAX];
    for (;;) {
        size_t len = xMessageBufferReceive(s_msgs, msg, sizeof(msg), portMAX_DELAY);
        if (len == 0) {
            continue;
        }
        switch (msg[0]) {
        case OP_BEGIN: begin(msg, len); break;
        case OP_DATA:  data(msg, len); break;
        case OP_END:   end(); break;
        case OP_ABORT:
            if (s_active) {
                fail(OP_ABORT, BLE_OTA_OK);
            }
            break;
        default: break;
        }
    }
}

// ---------------------------------------------------------------- GATT

static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t buf[MSG_MAX];
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 1 || len > sizeof(buf)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    os_mbuf_copydata(ctxt->om, 0, len, buf);
    // Never blocks the host: a client that ignores the window loses data, and
    // the update then fails on the next DATA or at END
    if (xMessageBufferSend(s_msgs, buf, len, 0) != len) {
        s_lost = true;
    }
    return 0;
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &CHR_UUID.u,
                .access_cb = chr_access,
                .val_handle = &s_chr_handle,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
            },
            {0},
        },
    },
    {0},
};

void ble_ota_gap_event(const struct ble_gap_event *event)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT: {
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        static const uint8_t abort_msg = OP_ABORT;   // an unfinished update is dropped
        if (xMessageBufferSend(s_msgs, &abort_msg, 1, 0) != 1) {
            s_lost = true;
        }
        break;
    }
    default:
        break;
    }
}

bool ble_ota_progress(int *percent, bool *restarting)
{
    uint32_t size = s_size;
    if (!s_active || size == 0) {
        return false;
    }
    *percent = (int)((uint64_t)s_written * 100 / size);
    *restarting = s_restarting;
    return true;
}

int ble_ota_init(void)
{
    s_msgs = xMessageBufferCreate(MSG_BUF_LEN);
    if (s_msgs == NULL || esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "ota", &s_pm_lock) != ESP_OK ||
        xTaskCreate(ota_task, "bleota", 4096, NULL, 3, NULL) != pdPASS) {
        return BLE_HS_ENOMEM;
    }
    int rc = ble_gatts_count_cfg(s_services);
    return rc ? rc : ble_gatts_add_svcs(s_services);
}
