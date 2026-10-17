#include "settings.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "rpm.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE "bxt"
#define NVS_KEY       "settings"
// Bump when settings_t changes layout; stale blobs are then ignored.
#define SETTINGS_VERSION 1

typedef struct {
    uint32_t version;
    settings_t s;
} settings_blob_t;

static settings_t s_settings;

void settings_reset(void)
{
    s_settings = (settings_t) {
        .rpm_thr_mv = RPM_THR_DEFAULT_MV,
        .edges_per_rev = CONFIG_BXT_EDGES_PER_REV,
        .load_cal_valid = false,
    };
}

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init");

    settings_reset();

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved settings, using defaults");
        return ESP_OK;
    }
    settings_blob_t blob;
    size_t len = sizeof(blob);
    err = nvs_get_blob(h, NVS_KEY, &blob, &len);
    nvs_close(h);

    if (err == ESP_OK && len == sizeof(blob) && blob.version == SETTINGS_VERSION) {
        s_settings = blob.s;
        ESP_LOGI(TAG, "loaded: thr=%u mV, edges/rev=%u, load cal=%s",
                 s_settings.rpm_thr_mv, s_settings.edges_per_rev,
                 s_settings.load_cal_valid ? "yes" : "no");
    } else {
        ESP_LOGW(TAG, "saved settings missing or outdated, using defaults");
    }
    return ESP_OK;
}

settings_t *settings_get(void)
{
    return &s_settings;
}

esp_err_t settings_save(void)
{
    settings_blob_t blob = {.version = SETTINGS_VERSION, .s = s_settings};
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_set_blob(h, NVS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
