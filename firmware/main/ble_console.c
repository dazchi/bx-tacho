// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
#include "ble_console.h"

#include <stdio.h>
#include <string.h>
#include "app.h"
#include "cli.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "host/ble_hs.h"

static const char *TAG = "blecon";

#define CMD_LINE_MAX      256
#define RX_BUF_LEN        512
#define TX_BUF_LEN        512
#define NOTIFY_RETRY_MS   10
#define NOTIFY_RETRIES    200     // ...while the stack is out of buffers: 2 s, then output is dropped
#define CODE_VALID_US     (120 * 1000000LL)   // a code is shown and accepted this long
#define WRONG_CODE_MS     1000    // before answering a wrong code: slows down guessing
#define CHAR_STOP         0x03
#define CHAR_READY        0x04
#define CHAR_HELLO        0x05    // from the client: send the banner, or ask for the code
#define CHAR_CODE         0x15    // to the client: enter the code on the display

// fcf6xxxx-7902-4817-adcc-c0380ba2609d, little-endian byte order
#define CON_UUID(b1, b0) BLE_UUID128_INIT(0x9d, 0x60, 0xa2, 0x0b, 0x38, 0xc0, 0xcc, 0xad, \
                                          0x17, 0x48, 0x02, 0x79, b0, b1, 0xf6, 0xfc)
static const ble_uuid128_t SVC_UUID = CON_UUID(0x00, 0x01);
static const ble_uuid128_t RX_UUID = CON_UUID(0x00, 0x02);
static const ble_uuid128_t TX_UUID = CON_UUID(0x00, 0x03);

static StreamBufferHandle_t s_rx;
static cli_console_t s_console;
static uint16_t s_tx_handle;

// Link state, written by the NimBLE host task
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_subscribed;

// The code on the display. Set by the console task; the host task clears both
// flags when the connection ends, so each connection needs the code again.
static volatile bool s_unlocked;
static volatile bool s_code_shown;
static volatile uint32_t s_code;
static volatile int64_t s_code_us;

// Output collected by the console task, sent in notifications of up to MTU - 3
static char s_tx[TX_BUF_LEN];
static size_t s_tx_len;

// ---------------------------------------------------------------- sending

static bool notify(const void *data, size_t len)
{
    for (int attempt = 0; attempt < NOTIFY_RETRIES; attempt++) {
        if (!s_subscribed || s_conn == BLE_HS_CONN_HANDLE_NONE) {
            return false;
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
        if (om != NULL) {
            int rc = ble_gatts_notify_custom(s_conn, s_tx_handle, om);   // consumes om
            if (rc == 0) {
                return true;
            }
            if (rc != BLE_HS_ENOMEM) {
                ESP_LOGW(TAG, "notify failed: %d", rc);
                return false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(NOTIFY_RETRY_MS));   // out of buffers: let the stack catch up
    }
    ESP_LOGW(TAG, "notify gave up: no buffers");
    return false;
}

static void tx_flush(void)
{
    size_t chunk = ble_att_mtu(s_conn);
    chunk = chunk > 3 ? chunk - 3 : 20;
    for (size_t at = 0; at < s_tx_len; at += chunk) {
        size_t n = s_tx_len - at < chunk ? s_tx_len - at : chunk;
        if (!notify(&s_tx[at], n)) {
            break;   // gone: drop the rest
        }
    }
    s_tx_len = 0;
}

// cli_console_t.write: whole lines go out at once, so output appears as it is printed
static void tx_write(const char *text, size_t len)
{
    while (len > 0) {
        size_t n = sizeof(s_tx) - s_tx_len;
        n = len < n ? len : n;
        memcpy(&s_tx[s_tx_len], text, n);
        s_tx_len += n;
        text += n;
        len -= n;
        if (s_tx_len == sizeof(s_tx)) {
            tx_flush();
        }
    }
    if (s_tx_len > 0 && s_tx[s_tx_len - 1] == '\n') {
        tx_flush();
    }
}

static void send_char(char c)
{
    tx_write(&c, 1);
    tx_flush();
}

// ---------------------------------------------------------------- the code

static bool code_valid(void)
{
    return s_code_shown && esp_timer_get_time() - s_code_us < CODE_VALID_US;
}

// Shows a code on the display (a new one unless `keep` and the current one is
// still good) and asks the client for it
static void ask_code(const char *why, bool keep)
{
    if (!keep || !code_valid()) {
        s_code = esp_random() % 1000000;
        s_code_us = esp_timer_get_time();
        s_code_shown = true;
        app_note_activity();   // wakes the display
    }
    cli_printf("%s\n", why);
    send_char(CHAR_CODE);
}

static void greet(void)
{
    cli_printf("BX Tacho v%s console over Bluetooth. Type 'help' for the commands.\n",
               esp_app_get_description()->version);
    send_char(CHAR_READY);
}

// A line typed while locked: the code, digits only ("123 456" works too)
static void check_code(const char *line)
{
    char want[8];
    char got[8];
    size_t n = 0;
    snprintf(want, sizeof(want), "%06lu", (unsigned long)s_code);
    for (; *line != '\0' && n < sizeof(got) - 1; line++) {
        if (*line != ' ') {
            got[n++] = *line;
        }
    }
    got[n] = '\0';

    bool valid = code_valid();
    if (valid && strcmp(got, want) == 0) {
        s_code_shown = false;
        s_unlocked = true;
        greet();
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(WRONG_CODE_MS));
    ask_code(valid ? "Wrong code. Enter the new one on BX Tacho's display."
                   : "That code has expired. Enter the new one on BX Tacho's display.", false);
}

// ---------------------------------------------------------------- task

static void console_task(void *arg)
{
    char line[CMD_LINE_MAX];
    size_t len = 0;
    bool overflow = false;

    cli_bind_console(&s_console);   // everything this task prints goes out over BLE
    for (;;) {
        char c;
        if (xStreamBufferReceive(s_rx, &c, 1, portMAX_DELAY) == 0) {
            continue;
        }
        if (c == CHAR_HELLO) {
            if (s_unlocked) {
                greet();
            } else {
                ask_code("Enter the code shown on BX Tacho's display.", true);
            }
            continue;
        }
        if (c != '\n' && c != '\r') {
            if (len < sizeof(line) - 1) {
                line[len++] = c;
            } else {
                overflow = true;
            }
            continue;
        }
        if (len == 0 && !overflow) {
            continue;   // the '\n' of "\r\n", or an empty line
        }
        line[len] = '\0';
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
            // gone meanwhile
        } else if (!s_unlocked) {
            check_code(line);
        } else {
            if (overflow) {
                cli_printf("Line too long (max %d characters).\n", CMD_LINE_MAX - 1);
            } else {
                cli_run_line(line);
            }
            send_char(CHAR_READY);
        }
        len = 0;
        overflow = false;
    }
}

// ---------------------------------------------------------------- GATT

static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const esp_app_desc_t *app = esp_app_get_description();
        int rc = os_mbuf_append(ctxt->om, app->project_name, strlen(app->project_name));
        rc = rc ? rc : os_mbuf_append(ctxt->om, " ", 1);
        rc = rc ? rc : os_mbuf_append(ctxt->om, app->version, strlen(app->version));
        // The address too: macOS never tells apps a device's real one
        uint8_t mac[6];
        char addr[20];
        if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
            int n = snprintf(addr, sizeof(addr), " %02X:%02X:%02X:%02X:%02X:%02X",
                             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            rc = rc ? rc : os_mbuf_append(ctxt->om, addr, n);
        }
        if (settings_flip()) {
            rc = rc ? rc : os_mbuf_append(ctxt->om, " flip", 5);
        }
        return rc ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t buf[64];
    uint16_t total = OS_MBUF_PKTLEN(ctxt->om);
    for (uint16_t at = 0; at < total;) {
        uint16_t n = total - at < sizeof(buf) ? total - at : sizeof(buf);
        os_mbuf_copydata(ctxt->om, at, n, buf);
        at += n;
        // Ctrl-C acts at once, even while the console task is busy running a command
        if (s_unlocked && memchr(buf, CHAR_STOP, n) != NULL) {
            s_console.stop = true;
        }
        for (uint16_t i = 0; i < n; i++) {
            if (buf[i] != CHAR_STOP && xStreamBufferSend(s_rx, &buf[i], 1, 0) != 1) {
                ESP_LOGW(TAG, "input buffer full, input dropped");
                return BLE_ATT_ERR_INSUFFICIENT_RES;
            }
        }
    }
    return 0;
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &RX_UUID.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &TX_UUID.u,
                .access_cb = chr_access,
                .val_handle = &s_tx_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ,
            },
            {0},
        },
    },
    {0},
};

void ble_console_gap_event(const struct ble_gap_event *event)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_subscribed = false;
            s_unlocked = false;
            s_code_shown = false;
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        s_unlocked = false;
        s_code_shown = false;
        s_console.stop = true;   // nobody is watching the running command any more
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_handle) {
            s_subscribed = event->subscribe.cur_notify;
        }
        break;
    default:
        break;
    }
}

bool ble_console_code(uint32_t *code)
{
    if (!code_valid()) {
        return false;
    }
    *code = s_code;
    return true;
}

bool ble_console_unlocked(void)
{
    return s_unlocked && s_conn != BLE_HS_CONN_HANDLE_NONE;
}

int ble_console_init(void)
{
    s_console.write = tx_write;
    s_rx = xStreamBufferCreate(RX_BUF_LEN, 1);
    if (s_rx == NULL || xTaskCreate(console_task, "blecon", 6144, NULL, 3, NULL) != pdPASS) {
        return BLE_HS_ENOMEM;
    }
    int rc = ble_gatts_count_cfg(s_services);
    return rc ? rc : ble_gatts_add_svcs(s_services);
}
