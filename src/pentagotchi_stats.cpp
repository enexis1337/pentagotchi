#include "pentagotchi_stats.h"

#include "pentagotchi_internal.h"

#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>

using namespace pentagotchi::detail;

static const char *TAG = "pentagotchi_stats";
static const char *kNamespace = "pentagotchi";
static const char *kKeyAps = "total_aps";
static const char *kKeyPwnd = "total_pwnd";

void pentagotchi_stats_load(pentagotchi_stats_t *stats) {
    if (!stats) { return; }
    stats->total_aps = 0;
    stats->total_pwnd = 0;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase, resetting");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return;
    }

    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS namespace");
        return;
    }
    nvs_get_u32(handle, kKeyAps, &stats->total_aps);
    nvs_get_u32(handle, kKeyPwnd, &stats->total_pwnd);
    nvs_close(handle);

    ESP_LOGI(TAG, "Loaded stats: aps=%lu pwnd=%lu", (unsigned long)stats->total_aps,
             (unsigned long)stats->total_pwnd);
    SERIAL_PRINTF("[pentagotchi] stats loaded: aps=%lu pwnd=%lu\n", (unsigned long)stats->total_aps,
                  (unsigned long)stats->total_pwnd);
}

bool pentagotchi_stats_save(const pentagotchi_stats_t *stats) {
    if (!stats) { return false; }

    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS namespace for writing");
        return false;
    }
    ESP_ERROR_CHECK(nvs_set_u32(handle, kKeyAps, stats->total_aps));
    ESP_ERROR_CHECK(nvs_set_u32(handle, kKeyPwnd, stats->total_pwnd));
    bool ok = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);

    if (ok) {
        ESP_LOGI(TAG, "Saved stats: aps=%lu pwnd=%lu", (unsigned long)stats->total_aps,
                 (unsigned long)stats->total_pwnd);
        SERIAL_PRINTF("[pentagotchi] stats saved: aps=%lu pwnd=%lu\n", (unsigned long)stats->total_aps,
                      (unsigned long)stats->total_pwnd);
    } else {
        ESP_LOGW(TAG, "Failed to commit stats to NVS");
    }
    return ok;
}