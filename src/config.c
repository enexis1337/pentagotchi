/**
 * config.c — загрузка/сохранение конфига с SD
 *
 * JSON-структура зеркалит секции pwnagotchi config.toml:
 *   { "main": {...}, "ui": { "display": {...}, "web": {...} },
 *     "ai": {...}, "pwny": {...} }
 *
 * Использует ArduinoJson (кроссплатформенная библиотека).
 */

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include <ArduinoJson.h>
#include "config.h"

static const char *TAG = "config";
#define CONFIG_PATH "/sdcard/config.json"

// ---------- Дефолты: безопасные, whitelist ПУСТОЙ ----------
static void config_set_defaults(pwny_config_t *cfg)
{
    memset(cfg, 0, sizeof(pwny_config_t));

    // [main]
    strncpy(cfg->main.name, "pentagotchi", CONFIG_STR_LEN_SHORT - 1);
    strncpy(cfg->main.lang, "ru", sizeof(cfg->main.lang) - 1);
    cfg->main.whitelist_count = 0;
    cfg->main.plugin_count = 0;

    // [ui]
    cfg->ui.fps = 1.0f;  // для e-ink — медленное обновление по умолчанию

    // [ui.display]
    cfg->ui.display.enabled = true;
    cfg->ui.display.rotation = 0;
    strncpy(cfg->ui.display.type, "weact_213", CONFIG_STR_LEN_SHORT - 1);
    strncpy(cfg->ui.display.color, "black", sizeof(cfg->ui.display.color) - 1);

    // [ui.web]
    cfg->ui.web.enabled = false;  // по умолчанию выключено — экономим ресурсы и батарею
    strncpy(cfg->ui.web.address, "0.0.0.0", CONFIG_STR_LEN_SHORT - 1);
    strncpy(cfg->ui.web.username, "admin", CONFIG_STR_LEN_SHORT - 1);
    strncpy(cfg->ui.web.password, "changeme", CONFIG_STR_LEN_SHORT - 1); // как у оригинала
    cfg->ui.web.port = 8080;

    // [ai]
    cfg->ai.enabled = false;   // по умолчанию AUTO-режим без обучения, включается осознанно
    cfg->ai.laziness = 0.5f;
    cfg->ai.epochs_per_episode = 1;
    cfg->ai.min_rssi = -75;

    // [pwny]
    strncpy(cfg->pwny.handshakes_path, "/sdcard/handshakes", CONFIG_STR_LEN_PATH - 1);
    cfg->pwny.silence_count = 0;
    cfg->pwny.deauth_enabled = false;
    cfg->pwny.channel_hop_ms = 300;
}

static esp_err_t parse_mac(const char *mac_str, uint8_t out[6])
{
    int vals[6];
    if (sscanf(mac_str, "%x:%x:%x:%x:%x:%x",
               &vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5]) != 6) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)vals[i];
    return ESP_OK;
}

// MAC-подобная строка ("AA:BB:CC:DD:EE:FF") как в whitelist оригинала
static bool looks_like_mac(const char *s)
{
    return strlen(s) >= 11 && s[2] == ':';
}

// ---------- Парсинг секции [main] ----------
static void parse_main_section(JsonObjectConst root, cfg_main_t *out)
{
    JsonObjectConst main_sec = root["main"];
    if (main_sec.isNull()) return;

    const char *name = main_sec["name"];
    if (name) {
        strncpy(out->name, name, CONFIG_STR_LEN_SHORT - 1);
    }

    const char *lang = main_sec["lang"];
    if (lang) {
        strncpy(out->lang, lang, sizeof(out->lang) - 1);
    }

    // main.whitelist — смешанный список SSID/MAC, как в оригинале
    JsonArrayConst whitelist = main_sec["whitelist"];
    if (!whitelist.isNull()) {
        for (JsonVariantConst item : whitelist) {
            if (out->whitelist_count >= CONFIG_MAX_WHITELIST) break;
            const char *str = item.as<const char*>();
            if (str) {
                strncpy(out->whitelist[out->whitelist_count], str, CONFIG_STR_LEN_SHORT - 1);
                out->whitelist_count++;
            }
        }
    }

    // main.plugins.<name>.enabled
    JsonObjectConst plugins = main_sec["plugins"];
    if (!plugins.isNull()) {
        for (JsonPairConst kv : plugins) {
            if (out->plugin_count >= CONFIG_MAX_PLUGINS) break;
            
            strncpy(out->plugin_name[out->plugin_count], kv.key().c_str(), CONFIG_STR_LEN_SHORT - 1);
            
            JsonObjectConst plugin_obj = kv.value();
            if (!plugin_obj.isNull()) {
                out->plugin_enabled[out->plugin_count] = plugin_obj["enabled"] | false;
            }
            out->plugin_count++;
        }
    }
}

// ---------- Парсинг секции [ui] ----------
static void parse_ui_section(JsonObjectConst root, cfg_ui_t *out)
{
    JsonObjectConst ui_sec = root["ui"];
    if (ui_sec.isNull()) return;

    if (ui_sec["fps"].is<float>()) {
        out->fps = ui_sec["fps"];
    }

    // ui.display
    JsonObjectConst display = ui_sec["display"];
    if (!display.isNull()) {
        if (display["enabled"].is<bool>()) {
            out->display.enabled = display["enabled"];
        }

        if (display["rotation"].is<int>()) {
            out->display.rotation = (uint8_t)display["rotation"].as<int>();
        }

        const char *type = display["type"];
        if (type) {
            strncpy(out->display.type, type, CONFIG_STR_LEN_SHORT - 1);
        }

        const char *color = display["color"];
        if (color) {
            strncpy(out->display.color, color, sizeof(out->display.color) - 1);
        }
    }

    // ui.web
    JsonObjectConst web = ui_sec["web"];
    if (!web.isNull()) {
        if (web["enabled"].is<bool>()) {
            out->web.enabled = web["enabled"];
        }

        const char *address = web["address"];
        if (address) {
            strncpy(out->web.address, address, CONFIG_STR_LEN_SHORT - 1);
        }

        const char *username = web["username"];
        if (username) {
            strncpy(out->web.username, username, CONFIG_STR_LEN_SHORT - 1);
        }

        const char *password = web["password"];
        if (password) {
            strncpy(out->web.password, password, CONFIG_STR_LEN_SHORT - 1);
        }

        if (web["port"].is<int>()) {
            out->web.port = (uint16_t)web["port"].as<int>();
        }
    }
}

// ---------- Парсинг секции [ai] ----------
static void parse_ai_section(JsonObjectConst root, cfg_ai_t *out)
{
    JsonObjectConst ai_sec = root["ai"];
    if (ai_sec.isNull()) return;

    if (ai_sec["enabled"].is<bool>()) {
        out->enabled = ai_sec["enabled"];
    }

    if (ai_sec["laziness"].is<float>()) {
        out->laziness = ai_sec["laziness"];
    }

    if (ai_sec["epochs_per_episode"].is<int>()) {
        out->epochs_per_episode = (uint16_t)ai_sec["epochs_per_episode"].as<int>();
    }

    if (ai_sec["min_rssi"].is<int>()) {
        out->min_rssi = (int8_t)ai_sec["min_rssi"].as<int>();
    }
}

// ---------- Парсинг секции [pwny] ----------
static void parse_pwny_section(JsonObjectConst root, cfg_pwny_t *out)
{
    JsonObjectConst pwny_sec = root["pwny"];
    if (pwny_sec.isNull()) return;

    const char *handshakes = pwny_sec["handshakes"];
    if (handshakes) {
        strncpy(out->handshakes_path, handshakes, CONFIG_STR_LEN_PATH - 1);
    }

    JsonArrayConst silence = pwny_sec["silence"];
    if (!silence.isNull()) {
        for (JsonVariantConst item : silence) {
            if (out->silence_count >= CONFIG_MAX_SILENCE_CHANNELS) break;
            if (item.is<int>()) {
                out->silence[out->silence_count++] = (uint8_t)item.as<int>();
            }
        }
    }

    if (pwny_sec["deauth_enabled"].is<bool>()) {
        out->deauth_enabled = pwny_sec["deauth_enabled"];
    }

    if (pwny_sec["channel_hop_ms"].is<int>()) {
        out->channel_hop_ms = (uint16_t)pwny_sec["channel_hop_ms"].as<int>();
    }
}

esp_err_t config_load(pwny_config_t *out_config)
{
    config_set_defaults(out_config);

    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "config.json not found, creating default at %s", CONFIG_PATH);
        config_save(out_config);
        return ESP_OK;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = (char *)malloc(size + 1);
    if (!buf) {
        fclose(f);
        ESP_LOGE(TAG, "OOM reading config");
        return ESP_ERR_NO_MEM;
    }
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);

    // Создаём документ для десериализации
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, buf);
    free(buf);

    if (error) {
        ESP_LOGE(TAG, "Failed to parse config.json: %s, using defaults", error.c_str());
        return ESP_ERR_INVALID_RESPONSE;
    }

    JsonObjectConst root = doc.as<JsonObjectConst>();

    parse_main_section(root, &out_config->main);
    parse_ui_section(root, &out_config->ui);
    parse_ai_section(root, &out_config->ai);
    parse_pwny_section(root, &out_config->pwny);

    ESP_LOGI(TAG, "Config loaded: name='%s' lang='%s' whitelist=%d plugins=%d "
                  "display=%s(%s) web=%s ai=%s deauth=%s",
             out_config->main.name, out_config->main.lang,
             out_config->main.whitelist_count, out_config->main.plugin_count,
             out_config->ui.display.enabled ? "on" : "off", out_config->ui.display.type,
             out_config->ui.web.enabled ? "on" : "off",
             out_config->ai.enabled ? "on" : "off",
             out_config->pwny.deauth_enabled ? "on" : "off");

    return ESP_OK;
}

esp_err_t config_save(const pwny_config_t *config)
{
    // Создаём JSON документ
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();

    // ---------- [main] ----------
    JsonObject main_sec = root["main"].to<JsonObject>();
    main_sec["name"] = config->main.name;
    main_sec["lang"] = config->main.lang;

    JsonArray whitelist = main_sec["whitelist"].to<JsonArray>();
    for (int i = 0; i < config->main.whitelist_count; i++) {
        whitelist.add(config->main.whitelist[i]);
    }

    JsonObject plugins = main_sec["plugins"].to<JsonObject>();
    for (int i = 0; i < config->main.plugin_count; i++) {
        JsonObject plugin_obj = plugins[config->main.plugin_name[i]].to<JsonObject>();
        plugin_obj["enabled"] = config->main.plugin_enabled[i];
    }

    // ---------- [ui] ----------
    JsonObject ui_sec = root["ui"].to<JsonObject>();
    ui_sec["fps"] = config->ui.fps;

    JsonObject display = ui_sec["display"].to<JsonObject>();
    display["enabled"] = config->ui.display.enabled;
    display["rotation"] = config->ui.display.rotation;
    display["type"] = config->ui.display.type;
    display["color"] = config->ui.display.color;

    JsonObject web = ui_sec["web"].to<JsonObject>();
    web["enabled"] = config->ui.web.enabled;
    web["address"] = config->ui.web.address;
    web["username"] = config->ui.web.username;
    web["password"] = config->ui.web.password;
    web["port"] = config->ui.web.port;

    // ---------- [ai] ----------
    JsonObject ai_sec = root["ai"].to<JsonObject>();
    ai_sec["enabled"] = config->ai.enabled;
    ai_sec["laziness"] = config->ai.laziness;
    ai_sec["epochs_per_episode"] = config->ai.epochs_per_episode;
    ai_sec["min_rssi"] = config->ai.min_rssi;

    // ---------- [pwny] ----------
    JsonObject pwny_sec = root["pwny"].to<JsonObject>();
    pwny_sec["handshakes"] = config->pwny.handshakes_path;

    JsonArray silence = pwny_sec["silence"].to<JsonArray>();
    for (int i = 0; i < config->pwny.silence_count; i++) {
        silence.add(config->pwny.silence[i]);
    }

    pwny_sec["deauth_enabled"] = config->pwny.deauth_enabled;
    pwny_sec["channel_hop_ms"] = config->pwny.channel_hop_ms;

    // ---------- запись на диск ----------
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", CONFIG_PATH);
        return ESP_FAIL;
    }

    // Сериализация с красивым форматированием
    if (serializeJsonPretty(doc, f) == 0) {
        fclose(f);
        ESP_LOGE(TAG, "Failed to write config to file");
        return ESP_FAIL;
    }

    fclose(f);

    ESP_LOGI(TAG, "Config saved to %s", CONFIG_PATH);
    return ESP_OK;
}

bool config_is_ssid_whitelisted(const pwny_config_t *config, const char *ssid)
{
    for (int i = 0; i < config->main.whitelist_count; i++) {
        if (looks_like_mac(config->main.whitelist[i])) continue; // это MAC-запись, не SSID
        if (strcmp(config->main.whitelist[i], ssid) == 0) {
            return true;
        }
    }
    return false;
}

bool config_is_bssid_whitelisted(const pwny_config_t *config, const uint8_t bssid[6])
{
    for (int i = 0; i < config->main.whitelist_count; i++) {
        if (!looks_like_mac(config->main.whitelist[i])) continue; // это SSID-запись, не MAC

        uint8_t parsed[6];
        if (parse_mac(config->main.whitelist[i], parsed) != ESP_OK) continue;

        if (memcmp(parsed, bssid, 6) == 0) {
            return true;
        }
    }
    return false;
}

bool config_is_channel_silenced(const pwny_config_t *config, uint8_t channel)
{
    for (int i = 0; i < config->pwny.silence_count; i++) {
        if (config->pwny.silence[i] == channel) return true;
    }
    return false;
}

bool config_is_plugin_enabled(const pwny_config_t *config, const char *plugin_name)
{
    for (int i = 0; i < config->main.plugin_count; i++) {
        if (strcmp(config->main.plugin_name[i], plugin_name) == 0) {
            return config->main.plugin_enabled[i];
        }
    }
    return false; // плагин не упомянут в конфиге -> выключен
}
