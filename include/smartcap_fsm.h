#pragma once

// SmartCap stage FSM. Pure logic driven by a tick(): every stage uses absolute
// wall-clock timestamps (now_ms) and edge checks (now >= deadline), never a
// "tick fires every N ms" assumption, so scheduler jitter cannot break the
// transitions.
//
// The FSM never touches the radio directly - all airtime goes through the
// smartcap_radio_t adapter (dry-run by default, see smartcap_radio.h), and all
// target knowledge comes from the smartcap_table_t fed by radio events.

#include "smartcap.h"    // smartcap_score_params_t
#include "smartcap_attack.h"
#include "smartcap_focus.h"
#include "smartcap_radio.h"
#include "smartcap_types.h"

#include <stdbool.h>
#include <stdint.h>

#define SMCAP_MAX_HS_SLOTS 8 // concurrent 4-way progress trackers

typedef enum {
    SMCAP_STAGE_SCAN = 0,    // passive dwell + channel hopping
    SMCAP_STAGE_RESCORE,     // timer gate: priorities are recomputed next stage
    SMCAP_STAGE_FOCUS,       // top-N selection + channel grouping
    SMCAP_STAGE_ATTACK,      // act on the current focus target
    SMCAP_STAGE_LISTEN,      // wait (bounded) for handshake / PMKID
    SMCAP_STAGE_COOLDOWN,    // pacing pause after a failed attempt
} smartcap_stage_t;

// All timing knobs live here so they can be tuned empirically without touching
// the transition logic.
typedef struct {
    uint32_t rescore_period_ms;    // how often priorities are recalculated
    uint32_t scan_period_ms;       // dwell per channel during normal scanning
    uint32_t scan_quick_period_ms; // dwell per channel while hunting (focus empty)
    uint32_t listen_timeout_ms;    // bound for LISTEN after an attack action
    uint32_t cooldown_ms;          // pause between a failed attempt and the next one
} smartcap_fsm_params_t;

typedef struct {
    uint64_t key;          // 6-byte BSSID (memcpy'd), 0 = free
    uint8_t seen;          // bit0..2: M1, M2, M3 observed
    uint32_t ts_ms;        // latest frame time (for slot eviction)
} smartcap_hs_slot_t;

// Optional stage-entry observer invoked by tick() whenever the machine
// transitions INTO `stage`. `param_ms` carries the stage's own duration when
// meaningful (SCAN dwell, LISTEN timeout, COOLDOWN pause), 0 otherwise. Keep
// the callback lightweight: it runs in the loop-task context.
typedef struct smartcap_fsm_s smartcap_fsm_t;
typedef void (*smartcap_fsm_cb_fn)(const smartcap_fsm_t *f, smartcap_stage_t stage,
                                   uint32_t param_ms, void *ctx);

typedef struct smartcap_fsm_s smartcap_fsm_t; // (see note above)
struct smartcap_fsm_s {
    // inputs
    smartcap_radio_t *radio;
    smartcap_table_t *table;
    smartcap_score_params_t *score;
    smartcap_fsm_params_t p;

    // stage state
    smartcap_stage_t stage;
    uint32_t rescore_next_ms;
    uint32_t stage_until_ms;   // generic absolute deadline for the current stage
    uint8_t scan_index;        // hopper index over the scan channel list
    uint8_t scan_hops;         // hops done on this hunt (fast scan)
    bool fast;                 // focus was empty last time -> quick channel hunt
    bool want_rescore;         // external request: leave SCAN and recompute now

    // focus bookkeeping
    smartcap_focus_t focus;
    uint8_t focus_index;
    const smartcap_target_t *current; // null outside ATTACK/LISTEN/COOLDOWN
    smartcap_strategy_t strategy;     // chosen for the current target
    uint32_t focus_started_ms;

    // 4-way handshake progress for arbitrary targets (passive captures too)
    smartcap_hs_slot_t hs[SMCAP_MAX_HS_SLOTS];

    // stage-entry observer (optional; firmware wiring)
    smartcap_fsm_cb_fn cb;
    void *cb_ctx;
};

void smartcap_fsm_params_default(smartcap_fsm_params_t *p);

// Prime the FSM into SCAN over `table`, driven by `radio`, scoring with
// `score`, using `p` as the timing set. now_ms = current clock.
void smartcap_fsm_begin(smartcap_fsm_t *f, smartcap_radio_t *radio,
                        smartcap_table_t *table, smartcap_score_params_t *score,
                        const smartcap_fsm_params_t *p, uint32_t now_ms);

// Advance the machine. Safe to call at any rate (even irregularly); the FSM
// only moves when deadlines are genuinely due. May make several transitions in
// one call (bounded) so it never observes a stale stage for more than a tick.
void smartcap_fsm_tick(smartcap_fsm_t *f, uint32_t now_ms);

smartcap_stage_t smartcap_fsm_stage(const smartcap_fsm_t *f);

// Install the optional stage-entry observer (see smartcap_fsm_cb_fn).
void smartcap_fsm_set_cb(smartcap_fsm_t *f, smartcap_fsm_cb_fn fn, void *ctx);

// Ask the FSM to leave SCAN early and recompute priorities (e.g. an important
// capture just landed and the current plan is already stale).
void smartcap_fsm_rescore(smartcap_fsm_t *f);