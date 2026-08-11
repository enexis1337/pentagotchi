#pragma once

// Fixed-size AP / client table for SmartCap. No dynamic allocation: entries are
// slots in a preallocated array, reused in place. When full, upsert() evicts
// the least valuable entry (lowest score; ties broken by oldest observation).

#include "smartcap_types.h"

#include <stdbool.h>

void smartcap_table_init(smartcap_table_t *t);

// Find an entry by BSSID, or claim a slot for a new BSSID (evicting the least
// valuable entry when the table is full). Never returns NULL while the entry
// array is a fixed part of the struct.
smartcap_target_t *smartcap_table_upsert(smartcap_table_t *t, const uint8_t *bssid);

smartcap_target_t *smartcap_table_find(smartcap_table_t *t, const uint8_t *bssid);

// Remove an entry (swap-with-last keeps the remaining slots' contents stable,
// so outstanding pointers to other entries stay valid).
bool smartcap_table_remove(smartcap_table_t *t, const uint8_t *bssid);

// Liveness bookkeeping for a received frame from the AP:
// EWMA-smoothed RSSI, channel (0 = keep previous) and last_seen_ms.
void smartcap_table_seen(smartcap_table_t *t, smartcap_target_t *target,
                         int8_t rssi, uint8_t channel, uint32_t now_ms);

// Remove a station from a target's client list. Used when the same MAC shows
// up talking to a different AP: a station belongs to exactly one AP at a time
// (multi-SSID routers appear as several BSSID entries), so the newest
// observation wins and the old AP's listing is dropped. Swap-with-last keeps
// the remaining slots' contents stable. Returns true if the MAC was present.
bool smartcap_table_remove_client(smartcap_target_t *target, const uint8_t *mac);

// Record a station talking to a target. Bounded at SMCAP_MAX_CLIENTS; once
// full, the least-recently-observed client slot is reclaimed. Membership is
// exclusive: if `mac` is currently listed under any *other* AP entry it is
// evicted there first (the freshest observation owns the station).
smartcap_client_t *smartcap_table_add_client(smartcap_table_t *t,
                                             smartcap_target_t *target,
                                             const uint8_t *mac, int8_t rssi,
                                             uint32_t now_ms);