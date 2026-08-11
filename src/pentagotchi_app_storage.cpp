#include "pentagotchi_app.h"
#include "eink_display.h"
#include "pentagotchi_gps.h"
#include "pentagotchi_internal.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <cstring>
#include <esp_log.h>

using namespace pentagotchi::detail;

static const String kHandshakeDir = "/Handshakes";

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

// Write a <name>.gps.json snapshot of the current GPS fix next to a .pcap.
static void writeGpsSidecar(const String &gpsPath) {
    const gps_fix_t *fx = gps_fix();
    if (!fx) { return; }

    fs::File file = SD.open(gpsPath, FILE_WRITE);
    if (!file) {
        ESP_LOGW(kLogTag, "Failed to open %s", gpsPath.c_str());
        SERIAL_PRINTF("[pentagotchi] Failed to open %s\n", gpsPath.c_str());
        return;
    }

    JsonDocument doc;
    doc["Latitude"] = fx->latitude;
    doc["Longitude"] = fx->longitude;
    doc["Altitude"] = fx->altitude;
    doc["Speed"] = fx->speed;

    char iso[24];
    snprintf(iso, sizeof(iso), "%04u-%02u-%02uT%02u:%02u:%02uZ",
             fx->year, fx->month, fx->day, fx->hour, fx->minute, fx->second);
    doc["Updated"] = iso;

    bool ok = serializeJsonPretty(doc, file) > 0;
    file.close();
    if (ok) {
        ESP_LOGI(kLogTag, "GPS snapshot saved to %s", gpsPath.c_str());
        SERIAL_PRINTF("[pentagotchi] GPS snapshot saved to %s\n", gpsPath.c_str());
    } else {
        ESP_LOGW(kLogTag, "Failed to write %s", gpsPath.c_str());
        SERIAL_PRINTF("[pentagotchi] Failed to write %s\n", gpsPath.c_str());
    }
}

void PentagotchiApp::ensureStorageReady() {
    if (storageReady) { return; }

    SPIClass *spi = display.spi();
    if (!spi) {
        ESP_LOGW(kLogTag, "SPI not initialized, cannot init SD");
        SERIAL_PRINTLN("[pentagotchi] SPI not initialized");
        return;
    }

    SERIAL_PRINTF("[pentagotchi] SD init: CS=GPIO%d, MOSI=GPIO%d, MISO=GPIO%d, SCK=GPIO%d\n",
                  PIN_SD_CS, PIN_SD_MOSI, PIN_SD_MISO, PIN_SD_SCK);

    if (!SD.begin(PIN_SD_CS, *spi)) {
        ESP_LOGW(kLogTag, "Failed to initialize SD card");
        SERIAL_PRINTLN("[pentagotchi] SD card initialization FAILED");
        return;
    }
    ESP_LOGI(kLogTag, "SD card initialized successfully");
    SERIAL_PRINTLN("[pentagotchi] SD card initialized successfully");

    storageReady = true;
    ESP_LOGI(kLogTag, "Handshake storage ready");
    SERIAL_PRINTLN("[pentagotchi] Handshake storage ready");
}

void PentagotchiApp::saveHandshake(const wifi_promiscuous_pkt_t *packet) {
    static uint32_t lastWarn = 0;
    if (!storageReady) {
        uint32_t now = millis();
        if (now - lastWarn > 5000) {
            ESP_LOGW(kLogTag, "Storage not ready; skipping handshake capture");
            SERIAL_PRINTLN("[pentagotchi] Storage not ready; skipping handshake capture");
            lastWarn = now;
        }
        return;
    }

    const uint8_t *addr1 = packet->payload + 4;
    const uint8_t *addr2 = packet->payload + 10;
    const uint8_t *bssid = packet->payload + 16;
    const uint8_t *apAddr = (memcmp(addr1, bssid, 6) == 0) ? addr1 : addr2;

    String macStr = macToString(apAddr);
    String macNum = macStr;
    macNum.replace(":", "");
    ensure_dir_recursive(kHandshakeDir);

    String path = kHandshakeDir;
    if (!path.endsWith("/")) { path += "/"; }
    path += "HS_" + macNum + ".pcap";

    fs::File file = SD.open(path, SD.exists(path) ? FILE_APPEND : FILE_WRITE);
    if (!file) {
        ESP_LOGW(kLogTag, "Failed to open %s", path.c_str());
        SERIAL_PRINTF("[pentagotchi] Failed to open %s\n", path.c_str());
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
    SERIAL_PRINTF("[pentagotchi] Handshake saved to %s (%u bytes)\n", path.c_str(), packet->rx_ctrl.sig_len);

    if (config_.gps_enabled) {
        if (gps_has_fix()) {
            String gpsPath = kHandshakeDir;
            if (!gpsPath.endsWith("/")) { gpsPath += "/"; }
            gpsPath += "HS_" + macNum + ".gps.json";
            writeGpsSidecar(gpsPath);
        } else {
            static uint32_t lastNoFixWarn = 0;
            uint32_t now = millis();
            if (now - lastNoFixWarn > 10000) {
                ESP_LOGW(kLogTag, "GPS enabled but no fix yet; skipping .gps.json for %s", path.c_str());
                lastNoFixWarn = now;
            }
        }
    }
}
