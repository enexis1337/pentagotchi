#pragma once

#include <Arduino.h>

#include <cstring>
#include <set>
#include <vector>

#include <esp_err.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <freertos/FreeRTOS.h>

class PwnagotchiApp;

namespace pwnagotchi::detail {

struct BeaconEntry {
    uint8_t mac[6]{};
    uint8_t channel{0};

    bool operator<(const BeaconEntry &other) const {
        int cmp = memcmp(mac, other.mac, sizeof(mac));
        if (cmp != 0) { return cmp < 0; }
        return channel < other.channel;
    }
};

struct PwngridPeer {
    String identity;
    String name;
    String face;
    int rssi{-1000};
    unsigned long lastPing{0};
    bool gone{false};
};

struct PcapGlobalHeader {
    uint32_t magic = 0xa1b2c3d4;
    uint16_t versionMajor = 2;
    uint16_t versionMinor = 4;
    int32_t thisZone = 0;
    uint32_t sigFigs = 0;
    uint32_t snapLen = 0xffff;
    uint32_t network = 105;
} __attribute__((packed));

struct PcapRecordHeader {
    uint32_t tsSec;
    uint32_t tsUsec;
    uint32_t inclLen;
    uint32_t origLen;
} __attribute__((packed));

// E-ink SPI bus (HSPI)
constexpr uint8_t PIN_EINK_MOSI = 15;
constexpr uint8_t PIN_EINK_SCK  = 17;

// SD card SPI bus (VSPI)
constexpr uint8_t PIN_SD_MOSI = 11;
constexpr uint8_t PIN_SD_MISO = 13;
constexpr uint8_t PIN_SD_SCK  = 12;
constexpr uint8_t PIN_SD_CS   = 5;

constexpr uint8_t kMaxPeers = 50;
constexpr uint32_t kUiRefreshMs = 500;
constexpr uint32_t kScanCycleMs = 3000;
constexpr uint32_t kMoodMinMs = 5000;
constexpr uint32_t kMoodMaxMs = 15000;
constexpr uint32_t kFullRefreshIntervalS = 1800;
constexpr uint8_t kTxPowerDefault = 72;

constexpr uint8_t kDeauthFrameTemplate[] = {0xc0, 0x00, 0x3a, 0x01, 0xff, 0xff, 0xff, 0xff,
                                            0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                            0x00, 0x00, 0x00, 0x00, 0xf0, 0xff, 0x02, 0x00};

constexpr wifi_promiscuous_filter_t kPromiscuousFilter = {
    .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA
};

extern portMUX_TYPE gRadioMux;
extern PwnagotchiApp *gInstance;
constexpr const char *kLogTag = "pwnagotchi";

extern std::set<BeaconEntry> gRegisteredBeacons;
extern std::vector<PwngridPeer> gPeers;
extern uint8_t gTotalFriends;
extern String gLastFriendName;
extern int gClosestRssi;
extern int gHandshakeCount;

inline String macToString(const uint8_t *mac) {
    char buffer[18];
    snprintf(
        buffer,
        sizeof(buffer),
        "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0],
        mac[1],
        mac[2],
        mac[3],
        mac[4],
        mac[5]
    );
    return String(buffer);
}

inline uint8_t readWifiChannel() {
    uint8_t primary = 1;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&primary, &second);
    return primary;
}

extern "C" esp_err_t esp_wifi_80211_tx(wifi_interface_t ifx, const void *buffer, int len, bool en_sys_seq);

} // namespace pwnagotchi::detail
