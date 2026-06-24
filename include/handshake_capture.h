/**
 * handshake_capture.h — захват WPA/WPA2 4-way handshake через promiscuous mode
 *
 * ПРИНЦИП РАБОТЫ:
 * 1. Включаем promiscuous mode, слушаем management + data фреймы
 * 2. Фильтруем EAPOL-Key фреймы (часть 802.1X) у целевого BSSID
 * 3. Когда собраны все 4 (или хотя бы M1+M2) — сохраняем в pcap
 *
 * ВАЖНО: целевые точки фильтруются через whitelist (config.h) —
 * если SSID/BSSID в whitelist, фреймы этой точки полностью игнорируются.
 */

#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "config.h"

typedef void (*handshake_captured_cb_t)(const uint8_t bssid[6], const char *ssid, const char *pcap_path);

/**
 * Инициализация захвата. Привязывает whitelist из конфига.
 */
esp_err_t handshake_capture_init(const pwny_config_t *config);

/**
 * Запускает promiscuous mode на указанном канале.
 */
esp_err_t handshake_capture_start(uint8_t channel);

void handshake_capture_stop(void);

/**
 * Регистрирует callback, который вызовется когда handshake полностью пойман.
 */
void handshake_capture_set_callback(handshake_captured_cb_t cb);
