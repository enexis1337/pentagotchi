// Isolated scoring engine for SmartCap. No ESP-IDF / Arduino dependencies by
// design - compile it on the host with plain g++ (see tools/run_smartcap_host_test.sh).

#include "smartcap.h"

#include <stdint.h>

void smartcap_score_params_default(smartcap_score_params_t *p) {
    p->base = 10;
    p->per_client = 25;
    p->rssi_floor = -92;
    p->rssi_ref = -40;
    p->rssi_weight = 30;
    p->novelty_bonus = 12;
    p->pmkid_bonus = 15;
    // Per consecutive failure. With cap=5 and window=60s, one failure costs ~60
    // and three in a row ~180 - enough to sink a 140+ raw score below the field
    // instead of merely shaving it (the old single-shot -60 did not).
    p->fail_penalty = 60;
    p->streak_penalty_cap = 5;
    p->fail_penalty_window_ms = 60000;
    // Hard exclusion: 5 consecutive failures pull the target out of rotation for
    // 30s, then 60s, 120s, ... capped at 10 min. A target that has not been
    // attacked for 15 min is treated as recovered and gets a clean slate.
    p->exclude_after_failures = 5;
    p->exclude_base_ms = 30000;
    p->exclude_max_ms = 600000;
    p->fail_reset_ms = 900000;
    p->recency_bonus = 8;
    p->recency_win_ms = 30000;
    p->close_flags = SMCAP_HAVE_HS | SMCAP_HAVE_PMKID;
}

bool smartcap_target_closed(const smartcap_score_params_t *p, const smartcap_target_t *t) {
    return p->close_flags != 0 && (t->flags & p->close_flags) != 0;
}

void smartcap_score_reconcile(const smartcap_score_params_t *p, smartcap_target_t *t,
                              uint32_t now_ms) {
    if (t->last_attack_ms != 0 && p->fail_reset_ms != 0) {
        const uint32_t elapsed =
            (now_ms >= t->last_attack_ms) ? (now_ms - t->last_attack_ms) : 0;
        if (elapsed >= p->fail_reset_ms) {
            // Long enough without an attempt that a client may have come back:
            // give the target a completely fresh start.
            t->consecutive_failures = 0;
            t->exclusion_count = 0;
            t->excluded_until_ms = 0;
        }
    }
}

int16_t smartcap_score(const smartcap_score_params_t *p, const smartcap_target_t *t, uint32_t now_ms) {

    // Closed targets carry full value already: score is meaningless, keep them
    // at 0 so table eviction naturally frees their slot too.
    if (!p || !t || smartcap_target_closed(p, t)) {
        return 0;
    }

    int32_t s = p->base;

    // Associated clients are the single strongest predictor of a successful
    // deauth handshake, so this term dominates by design.
    s += (int32_t)p->per_client * t->n_clients;

    // RSSI term: linear in [floor, ref], saturating at ref. Saturating is
    // intentional - -40 dBm should not score twice as high as -70 dBm.
    if (p->rssi_ref > p->rssi_floor) {
        const int32_t span = (int32_t)p->rssi_ref - p->rssi_floor;
        int32_t r = t->rssi;
        if (r < p->rssi_floor) r = p->rssi_floor;
        if (r > p->rssi_ref) r = p->rssi_ref;
        const int32_t term = ((int32_t)p->rssi_weight * (r - p->rssi_floor)) / span;
        s += (term > p->rssi_weight) ? p->rssi_weight : term;
    }

    // Novelty: a network we have never attacked is "fresh meat", worth probing
    // at least once even with no clients (it may yield a PMKID or a reconnect).
    if (t->attack_count == 0) {
        s += p->novelty_bonus;
    }

    // The silent method deserves a bonus, but deliberately kept below per_client
    // so it never outranks a target with real, attackable clients.
    if (t->flags & SMCAP_PMKID_METHOD) {
        s += p->pmkid_bonus;
    }

    // Recency keeps us biased toward channels with live traffic: a target heard
    // within the window gets a constant bonus instead of a stale-history scoring.
    if (p->recency_win_ms != 0 && t->last_seen_ms != 0 &&
        now_ms >= t->last_seen_ms &&
        (now_ms - t->last_seen_ms) < p->recency_win_ms) {
        s += p->recency_bonus;
    }

    // Cumulative failure-streak penalty. Unlike the old single-shot penalty
    // (which a strong multi-client target absorbed and shrugged off), this
    // multiplies with every consecutive failure and saturates at the cap, so a
    // persistently failing target sinks below fresher competition instead of
    // re-entering top-N forever. It fades linearly over fail_penalty_window_ms
    // and is cleared outright by capture / recovery / backoff-expiry.
    if (t->last_attack_ms != 0 && p->fail_penalty_window_ms != 0) {
        const uint32_t elapsed =
            (now_ms >= t->last_attack_ms) ? (now_ms - t->last_attack_ms) : 0;
        if (elapsed < p->fail_penalty_window_ms) {
            uint8_t streak = t->consecutive_failures;
            if (streak > p->streak_penalty_cap) {
                streak = p->streak_penalty_cap;
            }
            if (streak > 0) {
                const int32_t remaining = (int32_t)(p->fail_penalty_window_ms - elapsed);
                const int32_t pen =
                    ((int32_t)p->fail_penalty * streak * remaining) /
                    (int32_t)p->fail_penalty_window_ms;
                s -= pen;
            }
        }
    }

    if (s > 32767) s = 32767;
    if (s < -32767) s = -32767;
    return (int16_t)s;
}

int8_t smartcap_rssi_smooth(int8_t curr, int8_t sample, uint8_t alpha) {
    if (curr < -100) {
        return sample; // first or invalid sample: adopt it directly
    }
    const int32_t v = ((int32_t)curr * (255 - alpha) + (int32_t)sample * alpha) / 256;
    return (int8_t)v;
}