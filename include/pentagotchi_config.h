#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PWN_CONFIG_MAX_WHITELIST 16

// Settings loaded from /config.json on the SD card.
// All fields have safe defaults; a config file is optional.
typedef struct {
    // --- main ---
    char name[32];
    char lang[8];
    uint8_t whitelist[PWN_CONFIG_MAX_WHITELIST][6]; // MACs not attacked
    uint8_t whitelist_count;
    bool plugins_grid_enabled;
    bool plugins_gps_enabled;

    // --- ui ---
    struct {
        bool enabled;
        char rotation[12]; // "right" | "inverted" (others treated as right)
        char type[16];
        char color[8];
    } display;
    struct {
        bool enabled;
        char address[32];
        char username[24];
        char password[32];
    } web;

    // --- ai ---
    struct {
        bool enabled;
        float laziness;
        uint8_t epochs_per_episode;
        int16_t min_rssi;
    } ai;

    // --- pwny ---
    char saveDirectory[64];
    bool deauth_enabled;
    bool serial; // 0 = serial silent, 1 = serial output enabled
} pentagotchi_config_t;

void pentagotchi_config_set_defaults(pentagotchi_config_t *cfg);
bool pentagotchi_config_load(pentagotchi_config_t *cfg, bool sd_ready);
bool pentagotchi_config_save(const pentagotchi_config_t *cfg, bool sd_ready);

#ifdef __cplusplus
}
#endif