// Deterministic attack-strategy picker for SmartCap. Pure logic.

#include "smartcap_attack.h"

#include "smartcap_types.h"

smartcap_strategy_t smartcap_pick_strategy(const smartcap_target_t *target) {
    // Clients present -> the most reliable route is to kick one of them off
    // and catch its reassociation. Targeted (not broadcast) deauth by default.
    if (target->n_clients > 0) {
        return SMCAP_STRATEGY_DEAUTH_CLIENT;
    }
    // No clients but the AP cooperates with the silent PMKID method: cheaper
    // and less suspicious than any deauth, so prefer it over broadcasting.
    if (target->flags & SMCAP_PMKID_METHOD) {
        return SMCAP_STRATEGY_ASSOC_PMKID;
    }
    // Nothing usable: don't burn airtime, just keep an eye on it passively.
    return SMCAP_STRATEGY_PASSIVE;
}