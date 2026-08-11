// Target focus selection for SmartCap. Pure logic, host-compilable.

#include "smartcap_focus.h"

#include <stdio.h>
#include <string.h>

// Insertion sort in place. Primary: score desc. Secondary: BSSID ascending,
// purely to make the ordering deterministic across runs.
static void sortCandidates(smartcap_target_t **arr, size_t n) {
    for (size_t i = 1; i < n; ++i) {
        smartcap_target_t *key = arr[i];
        size_t j = i;
        while (j > 0) {
            const smartcap_target_t *a = arr[j - 1];
            const bool better =
                (key->score > a->score) ||
                (key->score == a->score && memcmp(key->bssid, a->bssid, 6) < 0);
            if (!better) {
                break;
            }
            arr[j] = const_cast<smartcap_target_t *>(a);
            --j;
        }
        arr[j] = key;
    }
}

uint8_t smartcap_focus_build(smartcap_table_t *t,
                             const smartcap_score_params_t *p,
                             uint32_t now_ms,
                             smartcap_focus_t *out,
                             smartcap_focus_log_fn log,
                             void *ctx) {
    out->count = 0;

    // Pass 1: refresh every stored score from the current clock (this is the
    // periodic "rescore" the spec asks for - cheap, a few dozen targets) and
    // collect the candidates that are still open and worth anything. This pass
    // also owns the hard-exclusion safeguard and the recovery bookkeeping.
    smartcap_target_t *cand[SMCAP_MAX_AP];
    size_t nc = 0;
    for (uint8_t i = 0; i < t->count; ++i) {
        smartcap_target_t *e = &t->entries[i];

        // Recovery: no attack for fail_reset_ms -> clean slate.
        smartcap_score_reconcile(p, e, now_ms);

        // Serve out an expired exclusion: the window passed, so clear the
        // streak and let the target back into rotation. exclusion_count is kept
        // so its next offense earns a longer backoff.
        if (e->excluded_until_ms != 0 && now_ms >= e->excluded_until_ms) {
            e->consecutive_failures = 0;
            e->excluded_until_ms = 0;
        }

        // Hard exclusion: a failing streak at or past the threshold opens a
        // backoff window (doubling per completed period, capped). While under
        // the window the target is NOT a rotation candidate even if its raw
        // score is the highest in the table.
        if (e->consecutive_failures >= p->exclude_after_failures &&
            e->excluded_until_ms == 0) {
            uint32_t dur = p->exclude_base_ms;
            uint8_t n = e->exclusion_count;
            while (n > 0 && dur < p->exclude_max_ms) {
                dur = (dur > p->exclude_max_ms / 2) ? p->exclude_max_ms : dur * 2;
                --n;
            }
            e->excluded_until_ms = now_ms + dur;
            ++e->exclusion_count;
            if (log) {
                char line[96];
                snprintf(line, sizeof(line),
                         "target %02X:%02X:...:%02X excluded from rotation for %u ms "
                         "after %u consecutive failures",
                         e->bssid[0], e->bssid[1], e->bssid[5], (unsigned)dur,
                         (unsigned)e->consecutive_failures);
                log(ctx, line);
            }
        }
        if (e->excluded_until_ms != 0 && now_ms < e->excluded_until_ms) {
            continue; // under exclusion: not a candidate this build
        }

        e->score = smartcap_score(p, e, now_ms);
        if (e->score > 0) {
            cand[nc++] = e;
        }
    }

    // Pass 2: top focus targets by score.
    sortCandidates(cand, nc);
    if (nc > SMCAP_MAX_FOCUS) {
        nc = SMCAP_MAX_FOCUS;
    }
    if (nc == 0) {
        return 0;
    }

    // Pass 3: group that shortlist by channel and rank channels by total score
    // so we do all the work on the most valuable channel first.
    typedef struct {
        uint8_t ch;
        int32_t sum;
    } channelInfo;
    channelInfo chs[SMCAP_MAX_FOCUS];
    uint8_t nch = 0;
    for (size_t i = 0; i < nc; ++i) {
        uint8_t k = 0;
        while (k < nch && chs[k].ch != cand[i]->channel) {
            ++k;
        }
        if (k == nch) {
            chs[nch].ch = cand[i]->channel;
            chs[nch].sum = cand[i]->score;
            ++nch;
        } else {
            chs[k].sum += cand[i]->score;
        }
    }
    // Insertion sort on channels: sum desc, then channel asc for determinism.
    for (uint8_t i = 1; i < nch; ++i) {
        const channelInfo key = chs[i];
        uint8_t j = i;
        while (j > 0) {
            const channelInfo a = chs[j - 1];
            const bool better = (key.sum > a.sum) || (key.sum == a.sum && key.ch < a.ch);
            if (!better) {
                break;
            }
            chs[j] = a;
            --j;
        }
        chs[j] = key;
    }

    // Pass 4: emit in execution order. The candidate list is already
    // score-sorted, so per-channel emission stays in score order too.
    for (uint8_t k = 0; k < nch && out->count < nc; ++k) {
        for (size_t i = 0; i < nc; ++i) {
            if (cand[i]->channel == chs[k].ch) {
                out->entries[out->count].target = cand[i];
                memcpy(out->entries[out->count].bssid, cand[i]->bssid, 6);
                out->count++;
            }
        }
    }
    return (uint8_t)nc;
}