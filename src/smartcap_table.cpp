// Fixed-size AP / client table for SmartCap. Pure logic, host-compilable.

#include "smartcap_table.h"
#include "smartcap.h"

#include <string.h>

void smartcap_table_init(smartcap_table_t *t) {
    memset(t, 0, sizeof(*t));
}

smartcap_target_t *smartcap_table_find(smartcap_table_t *t, const uint8_t *bssid) {
    for (uint8_t i = 0; i < t->count; ++i) {
        if (memcmp(t->entries[i].bssid, bssid, 6) == 0) {
            return &t->entries[i];
        }
    }
    return NULL;
}

smartcap_target_t *smartcap_table_upsert(smartcap_table_t *t, const uint8_t *bssid) {
    smartcap_target_t *e = smartcap_table_find(t, bssid);
    if (e) {
        return e;
    }

    if (t->count < SMCAP_MAX_AP) {
        e = &t->entries[t->count++];
    } else {
        // Table full: evict the least valuable entry. Score is primary (closed
        // targets sit at score 0, so they are reclaimed first); ties fall back
        // to the oldest observation so a recently-heard fresh network survives.
        e = &t->entries[0];
        for (uint8_t i = 1; i < t->count; ++i) {
            const smartcap_target_t *c = &t->entries[i];
            if (c->score < e->score ||
                (c->score == e->score && c->last_seen_ms < e->last_seen_ms)) {
                e = &t->entries[i];
            }
        }
    }

    memset(e, 0, sizeof(*e));
    memcpy(e->bssid, bssid, 6);
    e->rssi = -127; // "not measured yet" sentinel consumed by smartcap_rssi_smooth
    return e;
}

bool smartcap_table_remove(smartcap_table_t *t, const uint8_t *bssid) {
    for (uint8_t i = 0; i < t->count; ++i) {
        if (memcmp(t->entries[i].bssid, bssid, 6) == 0) {
            t->entries[i] = t->entries[t->count - 1];
            --t->count;
            return true;
        }
    }
    return false;
}

void smartcap_table_seen(smartcap_table_t *t, smartcap_target_t *target,
                         int8_t rssi, uint8_t channel, uint32_t now_ms) {
    (void)t;
    target->rssi = smartcap_rssi_smooth(target->rssi, rssi, 96);
    if (channel != 0) {
        target->channel = channel;
    }
    target->last_seen_ms = now_ms;
}

bool smartcap_table_remove_client(smartcap_target_t *target, const uint8_t *mac) {
    for (uint8_t i = 0; i < target->n_clients; ++i) {
        if (memcmp(target->clients[i].mac, mac, 6) == 0) {
            target->clients[i] = target->clients[target->n_clients - 1];
            --target->n_clients;
            return true;
        }
    }
    return false;
}

smartcap_client_t *smartcap_table_add_client(smartcap_table_t *t,
                                             smartcap_target_t *target,
                                             const uint8_t *mac, int8_t rssi,
                                             uint32_t now_ms) {
    // A station associates with exactly one AP at a time, and a single radio
    // may expose several BSSID entries (multi-SSID router). When the same MAC
    // is observed against a different AP, the freshest observation owns it:
    // evict it from every other AP's client list before recording it here, so
    // the FSM never attacks the same client against two BSSIDs at once.
    for (uint8_t i = 0; i < t->count; ++i) {
        smartcap_target_t *other = &t->entries[i];
        if (other != target) {
            smartcap_table_remove_client(other, mac);
        }
    }

    for (uint8_t i = 0; i < target->n_clients; ++i) {
        if (memcmp(target->clients[i].mac, mac, 6) == 0) {
            smartcap_client_t *c = &target->clients[i];
            c->rssi = smartcap_rssi_smooth(c->rssi, rssi, 96);
            c->last_seen_ms = now_ms;
            return c;
        }
    }

    if (target->n_clients < SMCAP_MAX_CLIENTS) {
        smartcap_client_t *c = &target->clients[target->n_clients++];
        memcpy(c->mac, mac, 6);
        c->rssi = rssi;
        c->last_seen_ms = now_ms;
        return c;
    }

    // Client slots full: keep the freshest set by reclaiming the least recent.
    smartcap_client_t *oldest = &target->clients[0];
    for (uint8_t i = 1; i < SMCAP_MAX_CLIENTS; ++i) {
        if (target->clients[i].last_seen_ms < oldest->last_seen_ms) {
            oldest = &target->clients[i];
        }
    }
    memcpy(oldest->mac, mac, 6);
    oldest->rssi = rssi;
    oldest->last_seen_ms = now_ms;
    return oldest;
}