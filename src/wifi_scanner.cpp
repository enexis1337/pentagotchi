/**
 * wifi_scanner.c — базовый passive scan точек доступа
 *
 * Это ШАГ 1: просто находим AP вокруг, без захвата handshake.
 * Promiscuous mode + EAPOL детект — следующий модуль (handshake_capture.c)
 */

#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "wifi_scanner.h"

static const char *TAG = "wifi_scanner";
static bool s_wifi_initialized = false;

esp_err_t wifi_scanner_init(void)
{
    if (s_wifi_initialized) {
        return ESP_OK;
    }

    // NVS нужен esp_wifi для хранения калибровок
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_wifi_initialized = true;
    ESP_LOGI(TAG, "WiFi scanner initialized");
    return ESP_OK;
}

int wifi_scanner_scan(ap_info_t *out_aps, int max_count)
{
    if (!s_wifi_initialized) {
        ESP_LOGE(TAG, "wifi_scanner_init() must be called first");
        return -1;
    }

    wifi_scan_config_t scan_config = {};
    scan_config.ssid = NULL;
    scan_config.bssid = NULL;
    scan_config.channel = 0;        // 0 = все каналы
    scan_config.show_hidden = true;
    scan_config.scan_type = WIFI_SCAN_TYPE_PASSIVE;
    scan_config.scan_time.passive = 200;  // мс на канал

    esp_err_t ret = esp_wifi_scan_start(&scan_config, true /* blocking */);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(ret));
        return -1;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    if (ap_count == 0) {
        ESP_LOGW(TAG, "No APs found");
        return 0;
    }

    wifi_ap_record_t *ap_records = (wifi_ap_record_t *)calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_records) {
        ESP_LOGE(TAG, "OOM allocating ap_records");
        return -1;
    }

    esp_wifi_scan_get_ap_records(&ap_count, ap_records);

    int n = (ap_count < max_count) ? ap_count : max_count;
    for (int i = 0; i < n; i++) {
        memcpy(out_aps[i].bssid, ap_records[i].bssid, 6);
        strncpy(out_aps[i].ssid, (char *)ap_records[i].ssid, 32);
        out_aps[i].ssid[32] = '\0';
        out_aps[i].channel = ap_records[i].primary;
        out_aps[i].rssi = ap_records[i].rssi;
        out_aps[i].authmode = ap_records[i].authmode;
    }

    free(ap_records);

    ESP_LOGI(TAG, "Scan complete: %d AP(s) found (showing %d)", ap_count, n);
    return n;
}
