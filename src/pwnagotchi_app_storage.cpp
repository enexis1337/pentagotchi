#include "pwnagotchi_app.h"
#include "eink_display.h"
#include "pwnagotchi_internal.h"

#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <cstring>
#include <esp_log.h>

using namespace pwnagotchi::detail;

static void ensure_dir_recursive(const String &path) {
    String cur;
    int start = 0;
while (start <= path.length()) {
        int slash = path.indexOf('/', start);
        if (slash < 0) { slash = path.length(); }
        String segment = path.substring(start, slash);
        if (!segment.isEmpty()) {
            if (!cur.isEmpty()) { cur += "/"; }
            cur += segment;
            if (!SD.exists(cur.c_str())) {
                SD.mkdir(cur.c_str());
            }
        }
        start = slash + 1;
    }
}

void PwnagotchiApp::ensureStorageReady() {
    if (storageReady) { return; }

    SPIClass *spi = display.spi();
    if (!spi) {
        ESP_LOGW(kLogTag, "SPI not initialized, cannot init SD");
        Serial.println("[pwnagotchi] SPI not initialized");
        return;
    }

    Serial.printf("[pwnagotchi] SD init: CS=GPIO%d, MOSI=GPIO%d, MISO=GPIO%d, SCK=GPIO%d\n",
                  PIN_SD_CS, PIN_SD_MOSI, PIN_SD_MISO, PIN_SD_SCK);

    if (!SD.begin(PIN_SD_CS, *spi)) {
        ESP_LOGW(kLogTag, "Failed to initialize SD card");
        Serial.println("[pwnagotchi] SD card initialization FAILED");
        Serial.println("[pwnagotchi] Check wiring: CS->GPIO5, MOSI->GPIO11, MISO->GPIO13, SCK->GPIO12");
        Serial.println("[pwnagotchi] Verify SD card is inserted and powered (3.3V)");
        return;
    }
    ESP_LOGI(kLogTag, "SD card initialized successfully");
    Serial.println("[pwnagotchi] SD card initialized successfully");

    storageReady = true;
    ESP_LOGI(kLogTag, "Handshake storage ready");
    Serial.println("[pwnagotchi] Handshake storage ready");
}

void PwnagotchiApp::saveHandshake(const wifi_promiscuous_pkt_t *packet) {
    static uint32_t lastWarn = 0;
    if (!storageReady) {
        uint32_t now = millis();
        if (now - lastWarn > 5000) {
            ESP_LOGW(kLogTag, "Storage not ready; skipping handshake capture");
            Serial.println("[pwnagotchi] Storage not ready; skipping handshake capture");
            lastWarn = now;
        }
        return;
    }

    const uint8_t *addr1 = packet->payload + 4;
    const uint8_t *addr2 = packet->payload + 10;
    const uint8_t *bssid = packet->payload + 16;
    const uint8_t *apAddr = (memcmp(addr1, bssid, 6) == 0) ? addr1 : addr2;

    String macStr = macToString(apAddr);
    const String saveDir = String(config.saveDirectory);
    ensure_dir_recursive(saveDir);

    String path = saveDir;
    if (!path.endsWith("/")) { path += "/"; }
    path += "HS_" + macStr;
    path.replace(":", "");
    path += ".pcap";

    fs::File file = SD.open(path, SD.exists(path) ? FILE_APPEND : FILE_WRITE);
    if (!file) {
        ESP_LOGW(kLogTag, "Failed to open %s", path.c_str());
        Serial.printf("[pwnagotchi] Failed to open %s\n", path.c_str());
        return;
    }

    if (file.size() == 0) {
        PcapGlobalHeader hdr;
        file.write(reinterpret_cast<const uint8_t *>(&hdr), sizeof(hdr));
    }

    PcapRecordHeader rec{};
    rec.tsSec = packet->rx_ctrl.timestamp / 1000000;
    rec.tsUsec = packet->rx_ctrl.timestamp % 1000000;
    rec.inclLen = packet->rx_ctrl.sig_len;
    rec.origLen = packet->rx_ctrl.sig_len;

    file.write(reinterpret_cast<const uint8_t *>(&rec), sizeof(rec));
    file.write(packet->payload, packet->rx_ctrl.sig_len);
    file.close();
    ESP_LOGI(kLogTag, "Handshake saved to %s (%u bytes)", path.c_str(), packet->rx_ctrl.sig_len);
    Serial.printf("[pwnagotchi] Handshake saved to %s (%u bytes)\n", path.c_str(), packet->rx_ctrl.sig_len);
}
