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

class PentagotchiApp {
public:
    explicit PentagotchiApp(EInkDisplay &display);

    void begin();
    void loop();
    void handleSerialCommands();

    EInkDisplay &display;
    volatile bool handshakePending{false};

private:
    void initWifi();
    void updateUi(bool fullRefresh);
    void rotateChannel();
    void performDeauthCycle();
    void ensureStorageReady();
    void saveHandshake(const wifi_promiscuous_pkt_t *packet);
    void updatePwnUiData();
    void updateUptime();

    static void wifiPromiscuousCallback(void *buf, wifi_promiscuous_pkt_type_t type);
    static bool isItEapol(const wifi_promiscuous_pkt_t *packet);
    uint8_t currentChannelIndex{0};
    uint32_t lastCycleTs{0};
    bool storageReady{false};
    bool deauthEnabled{false};
    uint32_t startTime{0};
    pentagotchi_config_t config{};
    pentagotchi_stats_t stats{};
    uint32_t statsChanges{0};
    uint32_t lastStatsFlush{0};
};
