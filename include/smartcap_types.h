#pragma once

// Shared POD types for the SmartCap subsystem. Fixed limits only - no dynamic
// allocation anywhere: every structure lives in statically-sized arrays.

#include <stdint.h>

// Hard limits (tunable at compile time, not runtime).
#define SMCAP_MAX_AP       32 // entries in the AP table
#define SMCAP_MAX_CLIENTS  8  // tracked (non-AP) stations per AP
#define SMCAP_MAX_FOCUS    4  // targets in "focus" (1..4, per the spec)
#define SMCAP_SSID_MAX     33 // char buffer for an SSID (32 + NUL)

// Per-target flags.
enum {
    SMCAP_HAVE_HS      = 1u << 0, // full 4-way handshake captured
    SMCAP_HAVE_PMKID   = 1u << 1, // PMKID captured via the assoc method
    SMCAP_PMKID_METHOD = 1u << 2, // AP *answers* an assoc request carrying a
                                  // PMKID KDE (AKM 0x8) - the silent method
};

// One station observed talking to a target AP.
typedef struct {
    uint8_t mac[6];
    int8_t rssi;        // EWMA-smoothed (from smartcap_rssi_smooth)
    uint32_t last_seen_ms;
} smartcap_client_t;

// One observed access point / target.
//
// The only time-based mutation allowed is "seen"/"touched" liveness bookkeeping
// and the explicit attack markers (last_attack_ms / attack_count). Everything
// else is derived from these by the scoring and focus stages.
typedef struct {
    uint8_t bssid[6];
    char ssid[SMCAP_SSID_MAX];
    uint8_t channel;
    int8_t rssi;               // EWMA-smoothed; -127 sentinel = not measured yet
    uint8_t flags;             // bitwise OR of the SMCAP_* enum above
    uint32_t last_seen_ms;     // last frame of this AP we received (0 = never)
    uint32_t last_attack_ms;   // last attack timestamp (0 = never attacked)
    uint16_t attack_count;     // total attack attempts (for the novelty bonus)
    uint8_t consecutive_failures; // failed active attacks in a row; reset on a
                                  // capture, on recovery (fail_reset_ms) and when
                                  // a backoff exclusion period serves out
    uint8_t exclusion_count;      // completed backoff periods - doubles the next one
    uint32_t excluded_until_ms;   // absolute clock: no active attack before this
    uint8_t n_clients;
    smartcap_client_t clients[SMCAP_MAX_CLIENTS];
    int16_t score;             // last computed score (recomputed on a timer, not per-packet)
} smartcap_target_t;

// The whole table: a fixed array, count-managed. Entries are never relocated
// by upsert() (only the victim slot is reused), so pointers into this array are
// only invalidated by remove().
typedef struct {
    uint8_t count;
    smartcap_target_t entries[SMCAP_MAX_AP];
} smartcap_table_t;