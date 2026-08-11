#pragma once

// Scoring engine for the SmartCap subsystem.
//
// smartcap_score.cpp depends ONLY on this header and smartcap_types.h - no Wi-Fi
// stack, no OS timers. That keeps it host-compilable so the weights below can be
// tuned empirically off-device (see tools/smartcap_host_test.cpp).

#include "smartcap_types.h"

#include <stdbool.h>

// Every knob that influences the score. Tuning these is the whole point of
// isolating the engine; the logic itself is a plain deterministic formula.
typedef struct {
    int16_t base;            // value of a bare, just-seen target
    int16_t per_client;      // added once per associated (non-AP) client

    // RSSI term: >=0 points, linear between rssi_floor and rssi_ref,
    // saturating at rssi_weight once the signal reaches rssi_ref.
    int16_t rssi_floor;      // dBm: at/under this the term contributes 0
    int16_t rssi_ref;        // dBm: at this the term reaches rssi_weight
    int16_t rssi_weight;     // max points the RSSI term can contribute

    int16_t novelty_bonus;   // one-shot bonus while attack_count == 0
    int16_t pmkid_bonus;     // target accepts the silent assoc/PMKID method

    // Cumulative failure-streak penalty. Every consecutive failed attempt adds
    // another fail_penalty (up to streak_penalty_cap entries), and the running
    // total fades linearly to 0 over fail_penalty_window_ms since the last
    // attempt. A single-shot penalty could not sink a strong multi-client
    // target (its client/RSSI terms outweigh -60); a multiplied streak does.
    //   streak term = fail_penalty * min(consecutive_failures, cap) * fade()
    int16_t fail_penalty;           // score lost per consecutive failed attempt
    uint8_t streak_penalty_cap;     // failures at which the streak penalty saturates
    uint32_t fail_penalty_window_ms; // fade-out window for the accumulated penalty

    // Hard safeguard (enforced by the focus builder, independent of score):
    // once consecutive_failures reaches exclude_after_failures the target is
    // pulled out of the top-N rotation for a backoff period that doubles with
    // each completed period, capped at exclude_max_ms. When a period serves
    // out the streak clears and the target is reconsidered; a target with no
    // attack for fail_reset_ms fully recovers (streak + period counters).
    uint8_t exclude_after_failures; // streak that triggers the hard exclusion
    uint32_t exclude_base_ms;       // first exclusion duration
    uint32_t exclude_max_ms;        // cap on the growing exclusion duration
    uint32_t fail_reset_ms;         // no attempt this long -> full recovery

    // Recency: a constant bonus while the target was observed within the window.
    // Rewards staying on channels where there is recent, live activity.
    int16_t recency_bonus;
    uint32_t recency_win_ms;

    // Which SMCAP_* flags are enough to consider a target "closed" (out of
    // rotation). Default: HAVE_HS | HAVE_PMKID - either capture satisfies.
    uint8_t close_flags;
} smartcap_score_params_t;

// Fills p with the default (initial) weight set. These are starting points for
// empirical tuning - pick apart the formula and adjust live after field tests.
void smartcap_score_params_default(smartcap_score_params_t *p);

// True when the target already carries any flag in p->close_flags.
// Such targets are excluded from scoring, focus and rotation entirely.
bool smartcap_target_closed(const smartcap_score_params_t *p, const smartcap_target_t *t);

// Periodic bookkeeping run once per scoring pass (before smartcap_score): if
// the target has not been attacked for >= fail_reset_ms it is considered
// recovered and its streak / exclusion counters are cleared. Mutates the
// target, so the caller must own the table.
void smartcap_score_reconcile(const smartcap_score_params_t *p, smartcap_target_t *t,
                              uint32_t now_ms);

// Deterministic one-pass score. Returns 0 for closed targets.
//   + base
//   + per_client * n_clients
//   + rssi term (see params)
//   + novelty_bonus              if attack_count == 0
//   + pmkid_bonus                if flags & SMCAP_PMKID_METHOD
//   + recency_bonus              if observed within recency_win_ms
//   - linearly faded fail_penalty if (now - last_attack_ms) < cooldown_ms
int16_t smartcap_score(const smartcap_score_params_t *p, const smartcap_target_t *t, uint32_t now_ms);

// EWMA smoothing for RSSI. alpha in 0..255 (256 = take the new sample).
// Adopts `sample` immediately when `curr` is the -127 "unset" sentinel.
int8_t smartcap_rssi_smooth(int8_t curr, int8_t sample, uint8_t alpha);