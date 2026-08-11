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

void PentagotchiApp::saveHandshake(const HandshakeCapture &capture) {
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

    String macStr = macToString(capture.bssid);
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

    // Write the cached beacon first (per-AP SSID context), then M1..M4 in
    // capture order. All frames are FCS-free.
    auto writeFrame = [&](const uint8_t *data, uint16_t len, uint32_t tsSec, uint32_t tsUsec) {
        if (len == 0)
            return;
        PcapRecordHeader rec{};
        rec.tsSec = tsSec;
        rec.tsUsec = tsUsec;
        rec.inclLen = len;
        rec.origLen = len;
        file.write(reinterpret_cast<const uint8_t *>(&rec), sizeof(rec));
        file.write(data, len);
    };

    if (capture.beacon.len > 0) {
        writeFrame(capture.beacon.data, capture.beacon.len,
                   capture.beacon.tsSec, capture.beacon.tsUsec);
    }
    bool complete = capture.m1.len > 0 && capture.m2.len > 0 &&
                    capture.m3.len > 0 && capture.m4.len > 0;
    writeFrame(capture.m1.data, capture.m1.len, capture.m1.tsSec, capture.m1.tsUsec);
    writeFrame(capture.m2.data, capture.m2.len, capture.m2.tsSec, capture.m2.tsUsec);
    writeFrame(capture.m3.data, capture.m3.len, capture.m3.tsSec, capture.m3.tsUsec);
    writeFrame(capture.m4.data, capture.m4.len, capture.m4.tsSec, capture.m4.tsUsec);
    file.close();

    gLastHandshakeFile = path;
    ESP_LOGI(kLogTag, "Handshake saved to %s (%s, %d frames)", path.c_str(),
             complete ? "complete" : "partial", (capture.beacon.len ? 1 : 0) + 4);
    SERIAL_PRINTF("[pentagotchi] Handshake saved to %s (%s)\n",
                  path.c_str(), complete ? "complete M1-M4" : "partial");

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
