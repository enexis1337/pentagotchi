/**
 * handshake_capture.c — захват EAPOL фреймов в promiscuous mode
 *
 * Структура 802.11 фрейма (упрощённо, нас интересует data frame с EAPOL):
 *
 * [Radiotap header (added by ESP-IDF)] -- сами не парсим, esp_wifi даёт raw 802.11
 * [802.11 MAC header: frame_ctrl(2) duration(2) addr1(6) addr2(6) addr3(6) seq(2)]
 * [LLC/SNAP header: 8 bytes]
 * [EAPOL: ethertype 0x888E, далее EAPOL-Key frame]
 *
 * EAPOL-Key frame (часть, которая нам важна — Key Information field)
 * позволяет различить M1/M2/M3/M4 хендшейка по битам Install/ACK/MIC.
 */

#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "handshake_capture.h"
#include "pcap_writer.h"

static const char *TAG = "handshake_cap";

#define EAPOL_ETHERTYPE      0x888E
#define MAX_TRACKED_BSSID    16
#define HANDSHAKE_TIMEOUT_US (10 * 1000 * 1000) // 10 сек на сбор всех msg

// ---------- 802.11 заголовки (минимально нужные поля) ----------
typedef struct __attribute__((packed)) {
    uint16_t frame_ctrl;
    uint16_t duration;
    uint8_t  addr1[6]; // RA / DA
    uint8_t  addr2[6]; // TA / SA
    uint8_t  addr3[6]; // BSSID (обычно)
    uint16_t seq_ctrl;
} wifi_ieee80211_mac_hdr_t;

typedef struct __attribute__((packed)) {
    wifi_ieee80211_mac_hdr_t hdr;
    uint8_t payload[0];
} wifi_ieee80211_packet_t;

// Состояние захвата для одной точки доступа
typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    bool active;
    uint8_t messages_captured; // битовая маска: бит0=M1, бит1=M2 и т.д.
    int64_t first_seen_us;
} target_state_t;

static target_state_t s_targets[MAX_TRACKED_BSSID];
static int s_target_count = 0;
static pwny_config_t s_config;
static handshake_captured_cb_t s_callback = NULL;
static bool s_running = false;

// ---------- Вспомогательные функции ----------

static bool mac_equal(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

static target_state_t *find_or_create_target(const uint8_t bssid[6])
{
    for (int i = 0; i < s_target_count; i++) {
        if (mac_equal(s_targets[i].bssid, bssid)) {
            return &s_targets[i];
        }
    }
    if (s_target_count >= MAX_TRACKED_BSSID) {
        return NULL; // таблица полна, придётся подождать освобождения
    }
    target_state_t *t = &s_targets[s_target_count++];
    memcpy(t->bssid, bssid, 6);
    t->ssid[0] = '\0';
    t->active = true;
    t->messages_captured = 0;
    t->first_seen_us = esp_timer_get_time();
    return t;
}

/**
 * Определяет номер EAPOL-сообщения (1-4) по Key Information field.
 * Key Info находится в EAPOL-Key body, смещение зависит от версии,
 * для упрощения берём ключевые биты: Install, Key ACK, Key MIC, Secure.
 *
 * M1: ACK=1 MIC=0
 * M2: ACK=0 MIC=1 Secure=0
 * M3: ACK=1 MIC=1 Secure=1 Install=1
 * M4: ACK=0 MIC=1 Secure=1 Install=0
 */
static int classify_eapol_message(const uint8_t *eapol_body, size_t len)
{
    if (len < 4) return 0;

    // EAPOL-Key descriptor type at offset 0 should be 2 (RSN) or 254 (WPA)
    uint8_t descriptor_type = eapol_body[0];
    if (descriptor_type != 2 && descriptor_type != 254) {
        return 0; // не EAPOL-Key frame
    }

    uint16_t key_info = (eapol_body[1] << 8) | eapol_body[2];
    bool install = key_info & (1 << 6);
    bool ack     = key_info & (1 << 7);
    bool mic     = key_info & (1 << 8);
    bool secure  = key_info & (1 << 9);

    if (ack && !mic) return 1;                       // M1
    if (!ack && mic && !secure) return 2;             // M2
    if (ack && mic && secure && install) return 3;    // M3
    if (!ack && mic && secure && !install) return 4;  // M4

    return 0;
}

// ---------- Главный callback promiscuous mode ----------

static void wifi_sniffer_packet_handler(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_DATA) {
        return; // нас интересуют только data frames (EAPOL внутри них)
    }

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    wifi_ieee80211_packet_t *ipkt = (wifi_ieee80211_packet_t *)pkt->payload;
    wifi_ieee80211_mac_hdr_t *hdr = &ipkt->hdr;

    // BSSID обычно в addr3 для frames между STA и AP
    uint8_t *bssid = hdr->addr3;

    // --- whitelist проверка: пропускаем точки, которые трогать нельзя ---
    if (config_is_bssid_whitelisted(&s_config, bssid)) {
        return;
    }

    // payload после MAC header: LLC/SNAP (8 байт) + EtherType (2 байта)
    uint8_t *llc = ipkt->payload;
    size_t payload_len = pkt->rx_ctrl.sig_len - sizeof(wifi_ieee80211_mac_hdr_t);

    if (payload_len < 10) return; // слишком короткий для LLC+EAPOL

    uint16_t ethertype = (llc[6] << 8) | llc[7];
    if (ethertype != EAPOL_ETHERTYPE) {
        return; // не EAPOL, пропускаем
    }

    uint8_t *eapol_body = llc + 8 + 4; // пропускаем LLC(8) + EAPOL header(4: version,type,length x2)
    size_t eapol_body_len = payload_len - 12;

    int msg_num = classify_eapol_message(eapol_body, eapol_body_len);
    if (msg_num == 0) return;

    target_state_t *target = find_or_create_target(bssid);
    if (!target) {
        ESP_LOGW(TAG, "Target table full, dropping new BSSID");
        return;
    }

    // тротлим повторную обработку и протухшие сессии
    int64_t now = esp_timer_get_time();
    if (now - target->first_seen_us > HANDSHAKE_TIMEOUT_US) {
        target->messages_captured = 0;
        target->first_seen_us = now;
    }

    target->messages_captured |= (1 << (msg_num - 1));

    ESP_LOGI(TAG, "EAPOL M%d captured from " MACSTR, msg_num, MAC2STR(bssid));

    // пишем фрейм в pcap по ходу (не дожидаясь всех 4 — частичный handshake тоже ценен)
    pcap_writer_append_frame(bssid, (uint8_t *)hdr, pkt->rx_ctrl.sig_len, pkt->rx_ctrl.timestamp);

    // если собрали минимум M1+M2 (достаточно для hashcat -m 22000) — считаем успехом
    if ((target->messages_captured & 0x03) == 0x03) {
        ESP_LOGI(TAG, "Handshake (M1+M2) complete for " MACSTR, MAC2STR(bssid));
        if (s_callback) {
            char path[64];
            pcap_writer_get_path(bssid, path, sizeof(path));
            s_callback(bssid, target->ssid, path);
        }
        target->active = false; // готово, дальше не трогаем эту точку в этой сессии
    }
}

// ---------- Public API ----------

esp_err_t handshake_capture_init(const pwny_config_t *config)
{
    memcpy(&s_config, config, sizeof(pwny_config_t));
    memset(s_targets, 0, sizeof(s_targets));
    s_target_count = 0;
    ESP_LOGI(TAG, "Handshake capture initialized, whitelist entries: %d, silenced channels: %d",
             config->main.whitelist_count, config->pwny.silence_count);
    return ESP_OK;
}

esp_err_t handshake_capture_start(uint8_t channel)
{
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(false));

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_packet_handler));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));

    s_running = true;
    ESP_LOGI(TAG, "Promiscuous capture started on channel %d", channel);
    return ESP_OK;
}

void handshake_capture_stop(void)
{
    esp_wifi_set_promiscuous(false);
    s_running = false;
    ESP_LOGI(TAG, "Capture stopped");
}

void handshake_capture_set_callback(handshake_captured_cb_t cb)
{
    s_callback = cb;
}
