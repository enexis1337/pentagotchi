#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "eink_display.h"
#include "config.h"
#include "sd_card.h"
#include "wifi_scanner.h"
#include "handshake_capture.h"
#include "pcap_writer.h"

static const char *TAG = "main";

static pwny_config_t s_config;

static void on_handshake_captured(const uint8_t bssid[6], const char *ssid, const char *pcap_path)
{
    ESP_LOGI(TAG, "Handshake captured! SSID='%s' BSSID=%02X:%02X:%02X:%02X:%02X:%02X -> %s",
             ssid, bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], pcap_path);
}

static void channel_hop_task(void *arg)
{
    const uint8_t all_channels[] = {1, 6, 11};
    const int n_all = sizeof(all_channels) / sizeof(all_channels[0]);

    uint8_t channels[sizeof(all_channels)];
    int n_channels = 0;
    for (int i = 0; i < n_all; i++) {
        if (!config_is_channel_silenced(&s_config, all_channels[i])) {
            channels[n_channels++] = all_channels[i];
        }
    }
    if (n_channels == 0) {
        ESP_LOGW(TAG, "All channels are silenced! Nothing to scan.");
        vTaskDelete(NULL);
        return;
    }

    int idx = 0;
    while (1) {
        handshake_capture_start(channels[idx]);
        idx = (idx + 1) % n_channels;
        vTaskDelay(pdMS_TO_TICKS(s_config.pwny.channel_hop_ms));
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== pwny-s3 starting ===");

    s_config.debug.serial_enabled = true;

    if (s_config.debug.serial_enabled) {
        ESP_LOGI(TAG, "Serial debug enabled");
    }

    // --- 1. E-ink display init (ПЕРВЫМ - инициализирует SPI шину) ---
    ESP_LOGI(TAG, "Initializing E-ink display...");
    esp_err_t eink_ret = eink_init();

    eink_status_t status;
    memset(&status, 0, sizeof(status));
    status.display_initialized = (eink_ret == ESP_OK);
    strncpy(status.device_name, "pentagotchi", sizeof(status.device_name) - 1);

    // --- 2. SD card (ВТОРЫМ - использует уже инициализированную SPI) ---
    ESP_LOGI(TAG, "Mounting SD card...");
    esp_err_t sd_ret = sd_card_init();

    sd_card_info_t sd_info;
    sd_card_get_info(&sd_info);
    status.sd_initialized = sd_info.initialized;
    status.sd_size_mb = sd_info.size_mb;

    if (sd_ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed");
    } else {
        ESP_LOGI(TAG, "SD card mounted: %s %uMB", sd_info.type_name, (unsigned int)sd_info.size_mb);
    }

    // --- 3. Display boot status ---
    if (status.display_initialized) {
        eink_show_boot_status(&status);
        ESP_LOGI(TAG, "Boot status displayed on e-ink");
    }

    vTaskDelay(pdMS_TO_TICKS(2000));

    // --- 4. Config / whitelist ---
    ESP_LOGI(TAG, "Loading config...");
    config_load(&s_config);

    // Apply display orientation from config
    if (status.display_initialized && s_config.ui.display.enabled) {
        int rot = (strcmp(s_config.ui.display.orientation, "right") == 0) ? 180 : 0;
        eink_set_rotation(rot);
        eink_show_boot_status(&status);
    }

    ESP_LOGI(TAG, "Device name: '%s' | lang: %s | AI: %s | web UI: %s | display: %s",
             s_config.main.name, s_config.main.lang,
             s_config.ai.enabled ? "ON (learning)" : "OFF (AUTO mode)",
             s_config.ui.web.enabled ? "ON" : "OFF",
             s_config.ui.display.enabled ? s_config.ui.display.type : "disabled");

    if (s_config.main.whitelist_count == 0) {
        ESP_LOGW(TAG, "WHITELIST IS EMPTY!");
    }

    // --- 5. WiFi init + scan ---
    ESP_LOGI(TAG, "Initializing WiFi scanner...");
    wifi_scanner_init();

    ap_info_t aps[SCANNER_MAX_APS];
    int found = wifi_scanner_scan(aps, SCANNER_MAX_APS);
    ESP_LOGI(TAG, "Found %d access point(s):", found);
    for (int i = 0; i < found; i++) {
        bool wl = config_is_ssid_whitelisted(&s_config, aps[i].ssid) ||
                  config_is_bssid_whitelisted(&s_config, aps[i].bssid);
        ESP_LOGI(TAG, "  [%d] SSID='%s' ch=%d rssi=%d %s",
                 i, aps[i].ssid, aps[i].channel, aps[i].rssi,
                 wl ? "[WL]" : "");
    }

    // --- 6. PCAP writer ---
    pcap_writer_init(s_config.pwny.handshakes_path);

    // --- 7. Handshake capture ---
    handshake_capture_init(&s_config);
    handshake_capture_set_callback(on_handshake_captured);

    // --- 8. Channel-hopping ---
    xTaskCreate(channel_hop_task, "channel_hop", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "=== pwny-s3 running ===");
}
