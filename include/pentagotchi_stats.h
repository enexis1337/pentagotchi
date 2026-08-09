#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Cumulative counters persisted in ESP32 internal flash (NVS).
typedef struct {
    uint32_t total_aps;   // cumulative number of APs ever detected
    uint32_t total_pwnd;  // cumulative number of handshakes ever captured
} pentagotchi_stats_t;

// Load stats from NVS (falls back to zeros if nothing stored yet).
void pentagotchi_stats_load(pentagotchi_stats_t *stats);

// Persist stats to NVS.
bool pentagotchi_stats_save(const pentagotchi_stats_t *stats);

#ifdef __cplusplus
}
#endif