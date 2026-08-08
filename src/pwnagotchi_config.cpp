#include "pwnagotchi_config.h"

#include "pwnagotchi_internal.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <SD.h>
#include <cstring>
#include <esp_log.h>

using namespace pwnagotchi::detail;

static const char *kConfigPath = "/config.json";

static void copy_string(const char *src, char *dst, size_t dst_size) {
    if (!src || !dst || dst_size == 0) { return; }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static int hexToInt(char c) {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

static bool parse_mac(const char *s, uint8_t out[6]) {
    if (!s || !s[0]) { return false; }
    int len = 0;
    while (s[len]) { ++len; }
    if (len != 17) { return false; }
    for (int i = 0; i < 6; ++i) {
        int hi = hexToInt(s[i * 3]);
        int lo = hexToInt(s[i * 3 + 1]);
        if (hi < 0 || lo < 0 || s[i * 3 + 2] != ':') { return false; }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

void pwnagotchi_config_set_defaults(pwnagotchi_config_t *cfg) {
    if (!cfg) { return; }
    memset(cfg, 0, sizeof(*cfg));

    strncpy(cfg->name, "pentagotchi", sizeof(cfg->name) - 1);
    cfg->name[sizeof(cfg->name) - 1] = '\0';
    strncpy(cfg->lang, "en", sizeof(cfg->lang) - 1);
    cfg->lang[sizeof(cfg->lang) - 1] = '\0';
    cfg->whitelist_count = 0;
    cfg->plugins_grid_enabled = false;
    cfg->plugins_gps_enabled = false;

    cfg->display.enabled = true;
    strncpy(cfg->display.rotation, "right", sizeof(cfg->display.rotation) - 1);
    cfg->display.rotation[sizeof(cfg->display.rotation) - 1] = '\0';
    strncpy(cfg->display.type, "weact_bw", sizeof(cfg->display.type) - 1);
    cfg->display.type[sizeof(cfg->display.type) - 1] = '\0';
    strncpy(cfg->display.color, "white", sizeof(cfg->display.color) - 1);
    cfg->display.color[sizeof(cfg->display.color) - 1] = '\0';

    cfg->web.enabled = false;
    strncpy(cfg->web.address, "192.168.4.1", sizeof(cfg->web.address) - 1);
    cfg->web.address[sizeof(cfg->web.address) - 1] = '\0';
    strncpy(cfg->web.username, "admin", sizeof(cfg->web.username) - 1);
    cfg->web.username[sizeof(cfg->web.username) - 1] = '\0';
    strncpy(cfg->web.password, "pentagotchi", sizeof(cfg->web.password) - 1);
    cfg->web.password[sizeof(cfg->web.password) - 1] = '\0';

    cfg->ai.enabled = false;
    cfg->ai.laziness = 0.5f;
    cfg->ai.epochs_per_episode = 1;
    cfg->ai.min_rssi = -75;

    strncpy(cfg->saveDirectory, "/sdcard/handshakes", sizeof(cfg->saveDirectory) - 1);
    cfg->saveDirectory[sizeof(cfg->saveDirectory) - 1] = '\0';
    cfg->deauth_enabled = true;
}

static void parse_whitelist(pwnagotchi_config_t *cfg, const JsonObject &main) {
    cfg->whitelist_count = 0;
    if (!main["whitelist"].is<JsonArray>()) { return; }
    for (JsonVariant v : main["whitelist"].as<JsonArray>()) {
        if (cfg->whitelist_count >= PWN_CONFIG_MAX_WHITELIST) { break; }
        const char *mac = v.as<const char *>();
        if (mac && parse_mac(mac, cfg->whitelist[cfg->whitelist_count])) {
            ++cfg->whitelist_count;
        }
    }
}

bool pwnagotchi_config_load(pwnagotchi_config_t *cfg, bool sd_ready) {
    if (!cfg) { return false; }

    if (!sd_ready) {
        ESP_LOGW(kLogTag, "SD not ready, using default config");
        return false;
    }

    if (!SD.exists(kConfigPath)) {
        ESP_LOGW(kLogTag, "%s not found, using default config", kConfigPath);
        return false;
    }

    fs::File file = SD.open(kConfigPath, FILE_READ);
    if (!file) {
        ESP_LOGW(kLogTag, "Failed to open %s", kConfigPath);
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    if (err != DeserializationError::Ok) {
        ESP_LOGW(kLogTag, "Failed to parse %s: %s", kConfigPath, err.c_str());
        return false;
    }

    if (doc["main"].is<JsonObject>()) {
        JsonObject main_ = doc["main"];
        copy_string(main_["name"] | cfg->name, cfg->name, sizeof(cfg->name));
        copy_string(main_["lang"] | cfg->lang, cfg->lang, sizeof(cfg->lang));
        parse_whitelist(cfg, main_);
        if (main_["plugins"].is<JsonObject>()) {
            JsonObject plugins = main_["plugins"];
            cfg->plugins_grid_enabled = plugins["grid"]["enabled"] | false;
            cfg->plugins_gps_enabled = plugins["gps"]["enabled"] | false;
        }
    }

    if (doc["ui"].is<JsonObject>()) {
        JsonObject ui = doc["ui"];
        if (ui["display"].is<JsonObject>()) {
            JsonObject display = ui["display"];
            cfg->display.enabled = display["enabled"] | cfg->display.enabled;
            copy_string(display["rotation"] | cfg->display.rotation, cfg->display.rotation,
                        sizeof(cfg->display.rotation));
            copy_string(display["type"] | cfg->display.type, cfg->display.type, sizeof(cfg->display.type));
            copy_string(display["color"] | cfg->display.color, cfg->display.color, sizeof(cfg->display.color));
        }
        if (ui["web"].is<JsonObject>()) {
            JsonObject web = ui["web"];
            cfg->web.enabled = web["enabled"] | cfg->web.enabled;
            copy_string(web["address"] | cfg->web.address, cfg->web.address, sizeof(cfg->web.address));
            copy_string(web["username"] | cfg->web.username, cfg->web.username, sizeof(cfg->web.username));
            copy_string(web["password"] | cfg->web.password, cfg->web.password, sizeof(cfg->web.password));
        }
    }

    if (doc["ai"].is<JsonObject>()) {
        JsonObject ai = doc["ai"];
        cfg->ai.enabled = ai["enabled"] | cfg->ai.enabled;
        cfg->ai.laziness = ai["laziness"] | cfg->ai.laziness;
        cfg->ai.epochs_per_episode = ai["epochs_per_episode"] | cfg->ai.epochs_per_episode;
        cfg->ai.min_rssi = ai["min_rssi"] | cfg->ai.min_rssi;
    }

    if (doc["pwny"].is<JsonObject>()) {
        JsonObject pwny = doc["pwny"];
        copy_string(pwny["saveDirectory"] | cfg->saveDirectory, cfg->saveDirectory, sizeof(cfg->saveDirectory));
        cfg->deauth_enabled = pwny["deauth_enabled"] | cfg->deauth_enabled;
    }

    ESP_LOGI(kLogTag, "Loaded config from %s (name=%s lang=%s deauth=%d whitelist=%u)",
             kConfigPath, cfg->name, cfg->lang, cfg->deauth_enabled, cfg->whitelist_count);
    return true;
}

bool pwnagotchi_config_save(const pwnagotchi_config_t *cfg, bool sd_ready) {
    if (!cfg) { return false; }
    if (!sd_ready) {
        ESP_LOGW(kLogTag, "SD not ready, config not saved");
        return false;
    }

    fs::File file = SD.open(kConfigPath, FILE_WRITE);
    if (!file) {
        ESP_LOGW(kLogTag, "Failed to create %s", kConfigPath);
        return false;
    }

    JsonDocument doc;

    JsonObject main_ = doc["main"].to<JsonObject>();
    main_["name"] = cfg->name;
    main_["lang"] = cfg->lang;
    JsonArray whitelist = main_["whitelist"].to<JsonArray>();
    for (uint8_t i = 0; i < cfg->whitelist_count; ++i) {
        char mac[18];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 cfg->whitelist[i][0], cfg->whitelist[i][1], cfg->whitelist[i][2],
                 cfg->whitelist[i][3], cfg->whitelist[i][4], cfg->whitelist[i][5]);
        whitelist.add(mac);
    }
    JsonObject plugins = main_["plugins"].to<JsonObject>();
    plugins["grid"]["enabled"] = cfg->plugins_grid_enabled;
    plugins["gps"]["enabled"] = cfg->plugins_gps_enabled;

    JsonObject ui = doc["ui"].to<JsonObject>();
    JsonObject display = ui["display"].to<JsonObject>();
    display["enabled"] = cfg->display.enabled;
    display["rotation"] = cfg->display.rotation;
    display["type"] = cfg->display.type;
    display["color"] = cfg->display.color;
    JsonObject web = ui["web"].to<JsonObject>();
    web["enabled"] = cfg->web.enabled;
    web["address"] = cfg->web.address;
    web["username"] = cfg->web.username;
    web["password"] = cfg->web.password;

    JsonObject ai = doc["ai"].to<JsonObject>();
    ai["enabled"] = cfg->ai.enabled;
    ai["laziness"] = cfg->ai.laziness;
    ai["epochs_per_episode"] = cfg->ai.epochs_per_episode;
    ai["min_rssi"] = cfg->ai.min_rssi;

    JsonObject pwny = doc["pwny"].to<JsonObject>();
    pwny["saveDirectory"] = cfg->saveDirectory;
    pwny["deauth_enabled"] = cfg->deauth_enabled;

    bool ok = serializeJsonPretty(doc, file) > 0;
    file.close();
    if (ok) {
        ESP_LOGI(kLogTag, "Saved config to %s", kConfigPath);
    } else {
        ESP_LOGW(kLogTag, "Failed to write %s", kConfigPath);
    }
    return ok;
}