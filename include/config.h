/**
 * config.h — конфигурация устройства, по структуре аналогичной pwnagotchi
 * config.toml (main / ui.display / ui.web / ai / pwny секции),
 * только в JSON вместо TOML.
 *
 * Иерархия ключей сохранена 1-в-1 с оригиналом, чтобы было легко
 * сверяться с документацией pwnagotchi при добавлении новых опций.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_MAX_WHITELIST        32
#define CONFIG_MAX_SILENCE_CHANNELS 16
#define CONFIG_MAX_PLUGINS          16

#define CONFIG_STR_LEN_SHORT   32
#define CONFIG_STR_LEN_MEDIUM  64
#define CONFIG_STR_LEN_PATH    96

// ============================================================
// [main] — секция как в pwnagotchi: main.name, main.lang, main.whitelist
// ============================================================
typedef struct {
    char name[CONFIG_STR_LEN_SHORT];   // main.name
    char lang[8];                       // main.lang ("en", "ru", ...)

    // main.whitelist — элементы могут быть и SSID, и MAC (как в оригинале),
    // поэтому различаем по наличию ':' при парсинге
    char whitelist[CONFIG_MAX_WHITELIST][CONFIG_STR_LEN_SHORT];
    uint8_t whitelist_count;

    // main.plugins.<name>.enabled — простой набор флагов "имя -> включен"
    char plugin_name[CONFIG_MAX_PLUGINS][CONFIG_STR_LEN_SHORT];
    bool plugin_enabled[CONFIG_MAX_PLUGINS];
    uint8_t plugin_count;
} cfg_main_t;

// ============================================================
// [ui.display] — экран
// ============================================================
typedef struct {
    bool enabled;                       // ui.display.enabled
    uint8_t rotation;                   // ui.display.rotation (0/90/180/270)
    char type[CONFIG_STR_LEN_SHORT];    // ui.display.type ("weact_213" у нас)
    char color[16];                     // ui.display.color ("black"/"red"...)
} cfg_ui_display_t;

// ============================================================
// [ui.web] — веб-интерфейс
// ============================================================
typedef struct {
    bool enabled;                              // ui.web.enabled
    char address[CONFIG_STR_LEN_SHORT];        // ui.web.address ("0.0.0.0")
    char username[CONFIG_STR_LEN_SHORT];       // ui.web.username
    char password[CONFIG_STR_LEN_SHORT];       // ui.web.password
    uint16_t port;                              // ui.web.port
} cfg_ui_web_t;

typedef struct {
    float fps;                  // ui.fps — для e-ink держим <=1
    cfg_ui_display_t display;
    cfg_ui_web_t web;
} cfg_ui_t;

// ============================================================
// [ai] / [personality] — параметры RL-агента
// (в оригинале это personality.*, у нас условно объединено под ai.*
//  для простоты — имена полей оставлены узнаваемыми)
// ============================================================
typedef struct {
    bool enabled;                // ai.enabled — если false, работаем в AUTO без обучения
    float laziness;              // ai.laziness — влияет на скорость смены каналов (0..1)
    uint16_t epochs_per_episode; // ai.epochs_per_episode
    int8_t min_rssi;             // личность: минимальный сигнал для взаимодействия (дБм, напр. -75)
} cfg_ai_t;

// ============================================================
// [debug] — отладочные флаги
// ============================================================
typedef struct {
    bool serial_enabled;                 // debug.serial.enabled — логи в UART
} cfg_debug_t;

// ============================================================
// [pwny] — наш движок захвата (аналог bettercap-части у pwnagotchi)
// ============================================================
typedef struct {
    char handshakes_path[CONFIG_STR_LEN_PATH]; // pwny.handshakes, по умолч. "/sdcard/handshakes"

    uint8_t silence[CONFIG_MAX_SILENCE_CHANNELS]; // pwny.silence — игнорируемые каналы
    uint8_t silence_count;

    bool deauth_enabled;     // разрешить деаутентификацию для форсирования handshake
    uint16_t channel_hop_ms; // время удержания на канале при сканировании
} cfg_pwny_t;

// ============================================================
// Главная структура — объединяет все секции
// ============================================================
typedef struct {
    cfg_main_t  main;
    cfg_ui_t    ui;
    cfg_ai_t    ai;
    cfg_debug_t debug;
    cfg_pwny_t  pwny;
} pwny_config_t;

/**
 * Загружает конфиг с SD-карты (/sdcard/config.json).
 * Если файла нет — создаёт дефолтный (main.whitelist ПУСТОЙ осознанно!).
 */
esp_err_t config_load(pwny_config_t *out_config);

/**
 * Сохраняет текущий конфиг обратно на SD.
 */
esp_err_t config_save(const pwny_config_t *config);

/**
 * Проверка: находится ли SSID в main.whitelist.
 */
bool config_is_ssid_whitelisted(const pwny_config_t *config, const char *ssid);

/**
 * Проверка: находится ли BSSID (MAC точки) в main.whitelist.
 * В оригинале whitelist смешанный (SSID и MAC в одном списке) —
 * сохраняем эту же логику: сравниваем MAC-строку напрямую.
 */
bool config_is_bssid_whitelisted(const pwny_config_t *config, const uint8_t bssid[6]);

/**
 * Проверка: находится ли канал в pwny.silence (игнорировать при сканировании).
 */
bool config_is_channel_silenced(const pwny_config_t *config, uint8_t channel);

/**
 * Проверка: включен ли плагин с данным именем (main.plugins.<name>.enabled).
 * Если плагин не упомянут в конфиге — возвращает false (выключен по умолчанию).
 */
bool config_is_plugin_enabled(const pwny_config_t *config, const char *plugin_name);

#ifdef __cplusplus
}
#endif
