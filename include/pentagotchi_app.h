#pragma once

#include <Arduino.h>
#include <esp_wifi_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "pwn_ui.h"
#include "pentagotchi_config.h"
#include "pentagotchi_stats.h"
#include "pentagotchi_grid.h"

class EInkDisplay;

// A single captured 802.11 frame with FCS already stripped.
struct CapFrame {
    uint8_t data[256]{};
    uint16_t len{0};
    uint32_t tsSec{0};
    uint32_t tsUsec{0};
};

// One cached raw beacon (SSID context record for a handshake pcap).
struct BeaconFrame {
    uint8_t data[512]{};
    uint16_t len{0};
    uint32_t tsSec{0};
    uint32_t tsUsec{0};
};

// A complete 4-way handshake (M1..M4) plus the beacon that identifies the AP.
// Captured the way the Bruce firmware does it: M1/M2/M3 are buffered and the
// whole exchange is only written to disk once M4 arrives.
struct HandshakeCapture {
    uint8_t bssid[6]{};
    char ssid[33]{};
    CapFrame m1;
    CapFrame m2;
    CapFrame m3;
    CapFrame m4;
    BeaconFrame beacon; // optional leading record so the SSID is in the pcap
};

class PentagotchiApp {
public:
    explicit PentagotchiApp(EInkDisplay &display);

    void begin();
    void loop();
    void handleSerialCommands();

    EInkDisplay &display;
    volatile bool handshakePending{false};

    // Read-only access for external modules (JS plugin bridge, ...)
    const pentagotchi_config_t &config() const { return config_; }
    const pentagotchi_stats_t &stats() const { return stats_; }
    uint32_t uptimeSec() const { return startTime == 0 ? 0 : (millis() - startTime) / 1000; }

private:
    void initWifi();
    void updateUi(bool fullRefresh);
    void ensureStorageReady();
    void saveHandshake(const HandshakeCapture &capture);
    void updatePwnUiData();
    void updateUptime();

    static void wifiPromiscuousCallback(void *buf, wifi_promiscuous_pkt_type_t type);
    static bool isItEapol(const wifi_promiscuous_pkt_t *packet);
    uint32_t lastCycleTs{0};
    bool storageReady{false};
    uint32_t startTime{0};
    pentagotchi_config_t config_{};
    pentagotchi_stats_t stats_{};
    uint32_t statsChanges{0};
    uint32_t lastStatsFlush{0};
};
