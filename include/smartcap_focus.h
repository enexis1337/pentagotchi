#pragma once

// Target focus selection for SmartCap: pick the top-SMCAP_MAX_FOCUS *open*
// targets by score, then order them channel-first so the device works all
// interesting targets on one channel before switching away.

#include "smartcap.h"        // smartcap_score_params_t
#include "smartcap_types.h"

// A selected target (pointer into the caller's table, plus a copy of its BSSID
// taken at build time - the copy survives table eviction/slot reuse, so the
// FSM can always resolve the entry by BSSID later).
typedef struct {
    const smartcap_target_t *target;
    uint8_t bssid[6];
} smartcap_focus_entry_t;

// Fixed-size focus result, ordered for execution.
typedef struct {
    uint8_t count;
    smartcap_focus_entry_t entries[SMCAP_MAX_FOCUS];
} smartcap_focus_t;

// Optional diagnostic sink for focus-time events (hard exclusions). NULL = off.
typedef void (*smartcap_focus_log_fn)(void *ctx, const char *line);

// Recomputes stored scores from the table (single pass, now_ms clock), keeps
// the SMCAP_MAX_FOCUS highest >0-scored non-closed targets, then emits them
// grouped by channel: the channel whose selected targets sum to the highest
// score comes first, with its targets in score order, then the next channel.
// Returns the number of selected targets (0..SMCAP_MAX_FOCUS).
//
// Applies the hard-exclusion safeguard: a target whose consecutive-failure
// streak reaches p->exclude_after_failures is withheld from the selection for a
// backoff period regardless of its raw score (see smartcap_score_params_t).
// log/ctx are optional; exclusions that just opened are reported through them.
uint8_t smartcap_focus_build(smartcap_table_t *t,
                             const smartcap_score_params_t *p,
                             uint32_t now_ms,
                             smartcap_focus_t *out,
                             smartcap_focus_log_fn log,
                             void *ctx);