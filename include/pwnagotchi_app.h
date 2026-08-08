#pragma once

#include <Arduino.h>
#include <esp_wifi_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "pwn_ui.h"
#include "pwnagotchi_config.h"

class EInkDisplay;

class PwnagotchiApp {
public:
    explicit PwnagotchiApp(EInkDisplay &display);

    void begin();
    void loop();

    EInkDisplay &display;
    volatile bool handshakePending{false};

private:
    void initWifi();
    void wakeAnimation();
    void updateUi(bool fullRefresh);
    void rotateChannel();
    void performDeauthCycle();
    void ensureStorageReady();
    void saveHandshake(const wifi_promiscuous_pkt_t *packet);
    void triggerRandomMood();
    void updatePwnUiData();
    void updateUptime();

    static void wifiPromiscuousCallback(void *buf, wifi_promiscuous_pkt_type_t type);
    static bool isItEapol(const wifi_promiscuous_pkt_t *packet);
    uint8_t currentChannelIndex{0};
    uint32_t lastMoodSwitch{0};
    uint32_t lastCycleTs{0};
    uint32_t randomMoodInterval{10000};
    bool storageReady{false};
    bool deauthEnabled{false};
    uint32_t startTime{0};
    pwnagotchi_config_t config{};
};
