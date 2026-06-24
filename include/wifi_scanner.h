/**
 * wifi_scanner.h — passive scan точек доступа (AP discovery)
 */

#pragma once
#include <stdint.h>
#include "esp_wifi.h"

#define SCANNER_MAX_APS 32

typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    uint8_t channel;
    int8_t rssi;
    wifi_auth_mode_t authmode;
} ap_info_t;

/**
 * Инициализация WiFi в режиме станции (нужен для active scan).
 */
esp_err_t wifi_scanner_init(void);

/**
 * Выполняет один passive scan по всем каналам.
 * Результат пишется в out_aps, возвращает количество найденных точек.
 */
int wifi_scanner_scan(ap_info_t *out_aps, int max_count);
