#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "eink_display.h"
#include "pwn_ui.h"
#include "config.h"
#include "sd_card.h"
#include "wifi_scanner.h"
#include "handshake_capture.h"
#include "pcap_writer.h"
#include "pins.h"

static const char *TAG = "main";

static pwny_config_t s_config;

static void on_handshake_captured(const uint8_t bssid[6], const char *ssid, const char *pcap_path)
{
    ESP_LOGI(TAG, "Handshake captured! SSID='%s' BSSID=%02X:%02X:%02X:%02X:%02X:%02X -> %s",
             ssid, bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], pcap_path);
    pwn_ui_on_handshake();
    char buf[PWN_STR_LEN];
    snprintf(buf, sizeof(buf), "%d (%d)", 0, 0);
    pwn_ui_set_shakes(buf);
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
        {
            char buf[PWN_STR_LEN];
            snprintf(buf, sizeof(buf), "%02d", (int)channels[idx]);
            pwn_ui_set_channel(buf);
        }
        pwn_ui_commit();
        idx = (idx + 1) % n_channels;
        vTaskDelay(pdMS_TO_TICKS(s_config.pwny.channel_hop_ms));
    }
}

static void ghosting_task_fn(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(600000));  // 10 минут
        ESP_LOGI(TAG, "Anti-ghosting full refresh");
        pwn_ui_full_commit();
    }
}

static void uptime_task_fn(void *arg)
{
    char buf[PWN_STR_LEN];
    char prev[PWN_STR_LEN] = "";
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint64_t us = esp_timer_get_time();
        uint32_t sec = us / 1000000;
        int h = sec / 3600;
        int m = (sec % 3600) / 60;
        int s = sec % 60;
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
        
        if (strcmp(buf, prev) != 0) {
            strcpy(prev, buf);
            pwn_ui_set_uptime(buf);  // Это отметит REGION_UPTIME как dirty
            pwn_ui_commit();  // Обновит только dirty регионы
        }
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== pwny-s3 starting ===");

    s_config.debug.serial_enabled = true;

    if (s_config.debug.serial_enabled) {
        ESP_LOGI(TAG, "Serial debug enabled");
    }

    // --- 0. Init shared SPI bus (both SD + E-ink use SPI2_HOST) ---
    ESP_LOGI(TAG, "Initializing shared SPI bus...");
    spi_bus_config_t buscfg = {};
    buscfg.miso_io_num = PIN_SPI_MISO;
    buscfg.mosi_io_num = PIN_SPI_MOSI;
    buscfg.sclk_io_num = PIN_SPI_SCK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = 8192;
    esp_err_t spi_ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (spi_ret != ESP_OK && spi_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(spi_ret));
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    // --- 1. SD card ---
    ESP_LOGI(TAG, "Mounting SD card...");
    esp_err_t sd_ret = sd_card_init();

    sd_card_info_t sd_info;
    sd_card_get_info(&sd_info);

    if (sd_ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed");
    } else {
        ESP_LOGI(TAG, "SD card mounted: %s %uMB", sd_info.type_name, (unsigned int)sd_info.size_mb);
    }

    // --- 2. Config (читаем с SD до инита E-ink, пока шина свободна) ---
    ESP_LOGI(TAG, "Loading config...");
    config_load(&s_config);

    vTaskDelay(pdMS_TO_TICKS(50));

    // --- 3. E-ink display (использует уже готовую SPI шину) ---
    ESP_LOGI(TAG, "Initializing E-ink display...");
    eink_init();
    eink_init_partial();

    // Apply display orientation from config (после инита u8g2)
    if (s_config.ui.display.enabled) {
        int rot = (strcmp(s_config.ui.display.orientation, "right") == 0) ? 180 : 0;
        eink_set_rotation(rot);
    }

    // --- 4. Init pwnagotchi-style UI with config values ---
    pwn_ui_init();
    pwn_ui_set_name(s_config.main.name);
    pwn_ui_set_mode(s_config.ai.enabled ? "  AI" : "AUTO");

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
    {
        char buf[PWN_STR_LEN];
        snprintf(buf, sizeof(buf), "%d", found);
        pwn_ui_set_aps(buf);
    }
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

    // --- 8. Show UI (first frame) ---
    pwn_ui_on_normal();

    // --- 9. Anti-ghosting refresh (every 60s) ---
    xTaskCreate(ghosting_task_fn, "ghosting", 2048, NULL, 2, NULL);

    // --- 10. Uptime counter (memory only, no display refresh) ---
    xTaskCreate(uptime_task_fn, "uptime", 2048, NULL, 3, NULL);

    // --- 11. Channel-hopping ---
    xTaskCreate(channel_hop_task, "channel_hop", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "=== pwny-s3 running ===");
}
