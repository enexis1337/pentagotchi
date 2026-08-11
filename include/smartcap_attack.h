#pragma once

// Per-target attack strategy selection for SmartCap. Deterministic heuristic -
// no machine learning, no randomness:

//   SMCAP_STRATEGY_DEAUTH_CLIENT - associated clients exist: deauth the least
//     noisy single client and wait for its reassociation (preferred).
//   SMCAP_STRATEGY_ASSOC_PMKID  - no clients, but the AP answers the silent
//     PMKID assoc method: pretend to be a client, no deauth noise at all.
//   SMCAP_STRATEGY_PASSIVE      - neither applies (no clients, no PMKID):
//     stay in the table at low priority and hope a client reconnects while we
//     happen to be listening on that channel anyway.
//   SMCAP_STRATEGY_DEAUTH_BCAST - reserved for a future "aggressive" operating
//     mode. Deauthing every station is louder than a targeted deauth, so the
//     picker below only ever returns it when explicitly requested to.

#include "smartcap_types.h"

typedef enum {
    SMCAP_STRATEGY_PASSIVE = 0,
    SMCAP_STRATEGY_DEAUTH_CLIENT,
    SMCAP_STRATEGY_DEAUTH_BCAST,
    SMCAP_STRATEGY_ASSOC_PMKID,
} smartcap_strategy_t;

smartcap_strategy_t smartcap_pick_strategy(const smartcap_target_t *target);