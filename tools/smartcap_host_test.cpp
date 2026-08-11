// Host test for the SmartCap scoring/table/focus/attack logic. No Wi-Fi stack,
// no ESP-IDF: builds with plain g++. Compile e.g. with:
//   g++ -std=gnu++17 -I include src/smartcap_score.cpp src/smartcap_table.cpp
//       src/smartcap_focus.cpp src/smartcap_attack.cpp tools/smartcap_host_test.cpp
// or just run tools/run_smartcap_host_test.sh.

#include "smartcap.h"
#include "smartcap_attack.h"
#include "smartcap_focus.h"
#include "smartcap_table.h"
#include "smartcap_types.h"

#include <stdio.h>
#include <string.h>

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failed;                                                      \
            printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg);          \
        }                                                                    \
    } while (0)

static void setMac(uint8_t out[6], uint8_t a, uint8_t b, uint8_t c) {
    out[0] = a; out[1] = b; out[2] = c;
    out[3] = 1; out[4] = 2; out[5] = 3;
}

static int targetScore(const smartcap_score_params_t *p,
                       uint8_t cli, int8_t rssi, uint8_t attCnt,
                       uint32_t lastSeen, uint32_t lastAttack, uint8_t flags) {
    smartcap_target_t t;
    memset(&t, 0, sizeof(t));
    t.n_clients = cli;
    t.rssi = rssi;
    t.attack_count = attCnt;
    t.consecutive_failures = attCnt; // in practice the streak tracks attempts
    t.last_seen_ms = lastSeen;
    t.last_attack_ms = lastAttack;
    t.flags = flags;
    const uint32_t now = 100000;
    return smartcap_score(p, &t, now);
}

static void testScore(void) {
    printf("score engine:\n");
    smartcap_score_params_t p;
    smartcap_score_params_default(&p);

    // Use RSSI below rssi_floor so the RSSI term is 0 and the base arithmetic
    // is easy to reason about.
    const int rssiOff = p.rssi_floor - 3;

    // Fresh, strong, recent, PMKID-capable target:
    // base + novelty + recency + pmkid (rssi term = 0 at rssiOff).
    const int fresh = targetScore(&p, 0, rssiOff, 0, 99999, 0, SMCAP_PMKID_METHOD);
    CHECK(fresh == p.base + p.novelty_bonus + p.recency_bonus + p.pmkid_bonus,
          "fresh target must accumulate all positive terms");

    // Per-client term scales linearly.
    const int oneCli = targetScore(&p, 1, rssiOff, 0, 99999, 0, 0);
    CHECK(oneCli == fresh - p.pmkid_bonus + p.per_client,
          "per_client term must be linear");

    // RSSI term saturates at rssi_ref (no runaway double-counting).
    const int loud = targetScore(&p, 0, -40, 0, 99999, 0, 0);
    const int louder = targetScore(&p, 0, -25, 0, 99999, 0, 0);
    CHECK(loud == louder, "rssi term must saturate at rssi_ref");
    const int weak = targetScore(&p, 0, p.rssi_floor, 0, 99999, 0, 0);
    CHECK(weak == p.base + p.novelty_bonus + p.recency_bonus,
          "rssi term must be zero at/below rssi_floor");

    // No recency bonus once the target went quiet past the window.
    const int stale = targetScore(&p, 0, rssiOff, 0, 1000, 0, 0);
    CHECK(fresh - stale == p.pmkid_bonus + p.recency_bonus,
          "stale target loses both the recency bonus and (only the pmkid stays)");

    // A prior attack removes the novelty bonus.
    const int probed = targetScore(&p, 0, rssiOff, 1, 99999, 0, 0);
    CHECK(probed == fresh - p.pmkid_bonus - p.novelty_bonus,
          "probed target loses the novelty bonus");

    // Failure streak penalty: cumulative (each failure adds another
    // fail_penalty) and saturating at the cap.
    const int now = 100000;
    const int f1 = targetScore(&p, 0, rssiOff, 1, 99999, now, 0);
    const int f2 = targetScore(&p, 0, rssiOff, 2, 99999, now, 0);
    const int f3 = targetScore(&p, 0, rssiOff, 3, 99999, now, 0);
    CHECK(f2 == f1 - p.fail_penalty && f3 == f2 - p.fail_penalty,
          "each consecutive failure adds one more fail_penalty");
    const int fCap = targetScore(&p, 0, rssiOff, p.streak_penalty_cap, 99999, now, 0);
    const int fMore = targetScore(&p, 0, rssiOff, p.streak_penalty_cap + 5, 99999, now, 0);
    CHECK(fMore == fCap, "streak penalty saturates at the cap");
    // Fade: fresh failures hurt most, the penalty cools linearly and fully
    // expires at the end of the window.
    const int hot = targetScore(&p, 0, rssiOff, 2, 99999, now - 1, 0);
    const int half = targetScore(&p, 0, rssiOff, 2, 99999, now - p.fail_penalty_window_ms / 2, 0);
    const int cooled = targetScore(&p, 0, rssiOff, 2, 99999, now - p.fail_penalty_window_ms - 1, 0);
    CHECK(hot < half, "fresh failure must penalize harder than an old one");
    CHECK(half < cooled, "penalty must fade toward zero during the window");
    CHECK(cooled == p.base + p.recency_bonus, "penalty must fully expire after the window");
    // Recovery bookkeeping: a target quiet past fail_reset_ms has no streak.
    {
        smartcap_target_t t;
        memset(&t, 0, sizeof(t));
        t.last_attack_ms = 1;
        t.consecutive_failures = 7;
        t.exclusion_count = 3;
        t.excluded_until_ms = 100000000;
        smartcap_score_reconcile(&p, &t, 1 + p.fail_reset_ms + 1);
        CHECK(t.consecutive_failures == 0 && t.exclusion_count == 0 &&
              t.excluded_until_ms == 0,
              "reconcile() fully clears a recovered target");
    }

    // Closed targets score 0.
    {
        smartcap_target_t t;
        memset(&t, 0, sizeof(t));
        t.flags = SMCAP_HAVE_HS;
        CHECK(smartcap_target_closed(&p, &t), "HAVE_HS target must be closed by default");
        CHECK(smartcap_score(&p, &t, now) == 0, "closed target scores 0");
    }
}

static void testSmoothed(void) {
    printf("rssi smoothing:\n");
    CHECK(smartcap_rssi_smooth(-127, -70, 96) == -70, "first sample adopted directly");
    CHECK(smartcap_rssi_smooth(-70, -50, 128) > -70, "second sample moves upward");
}

static void testTable(void) {
    printf("table:\n");
    smartcap_table_t T;
    smartcap_table_init(&T);
    CHECK(T.count == 0, "table starts empty");

    uint8_t a[6], b[6];
    setMac(a, 0xaa, 0xaa, 0xaa);
    setMac(b, 0xbb, 0xbb, 0xbb);

    smartcap_target_t *ta = smartcap_table_upsert(&T, a);
    CHECK(ta != NULL && T.count == 1, "upsert adds entry");
    CHECK(ta->rssi == -127, "new entry uses the unset RSSI sentinel");
    CHECK(smartcap_table_find(&T, a) == ta, "find returns the same slot");

    smartcap_table_seen(&T, ta, -60, 6, 1000);
    CHECK(ta->rssi == -60 && ta->channel == 6 && ta->last_seen_ms == 1000,
          "seen() records liveness");

    // Client bookkeeping, bounded + LRU reclaim.
    uint8_t cA[6], cB[6], cC[6], cD[6], cE[6], cF[6], cG[6], cH[6], cI[6];
    setMac(cA, 1, 1, 1); setMac(cB, 2, 2, 2); setMac(cC, 3, 3, 3);
    setMac(cD, 4, 4, 4); setMac(cE, 5, 5, 5); setMac(cF, 6, 6, 6);
    setMac(cG, 7, 7, 7); setMac(cH, 8, 8, 8); setMac(cI, 9, 9, 9);

    smartcap_table_add_client(&T, ta, cA, -60, 10000);
    smartcap_client_t *same = smartcap_table_add_client(&T, ta, cA, -58, 20000);
    CHECK(ta->n_clients == 1 && same == &ta->clients[0],
          "duplicate client MAC is updated, not duplicated");

    smartcap_table_add_client(&T, ta, cB, -60, 30001);
    smartcap_table_add_client(&T, ta, cC, -60, 30002);
    smartcap_table_add_client(&T, ta, cD, -60, 30003);
    smartcap_table_add_client(&T, ta, cE, -60, 30004);
    smartcap_table_add_client(&T, ta, cF, -60, 30005);
    smartcap_table_add_client(&T, ta, cG, -60, 30006);
    smartcap_table_add_client(&T, ta, cH, -60, 30007);
    CHECK(ta->n_clients == SMCAP_MAX_CLIENTS, "clients cap at the fixed limit");

    // Oldest now is cA (last seen 20000). Adding a ninth client reclaims it.
    smartcap_table_add_client(&T, ta, cI, -60, 40000);
    CHECK(ta->n_clients == SMCAP_MAX_CLIENTS, "full client list is reclaimed, not grown");
    CHECK(memcmp(ta->clients[0].mac, cI, 6) == 0,
          "oldest client slot is the reclaimed one");

    // Eviction: a full table evicts the lowest-score entry.
    smartcap_table_t T2;
    smartcap_table_init(&T2);
    for (uint8_t i = 0; i < SMCAP_MAX_AP; ++i) {
        uint8_t m[6];
        setMac(m, 0x40, 0x40, (uint8_t)i);
        smartcap_target_t *e = smartcap_table_upsert(&T2, m);
        e->score = (int16_t)(10 + i); // ascending 10..41
    }
    uint8_t newMac[6];
    setMac(newMac, 0x99, 0x99, 0x99);
    smartcap_target_t *newT = smartcap_table_upsert(&T2, newMac);
    CHECK(T2.count == SMCAP_MAX_AP, "table stayed at the cap");
    CHECK(memcmp(newT->bssid, newMac, 6) == 0, "new bssid owns the reclaimed slot");

    // The evicted bssid must have been the lowest score (10): find it -> gone.
    uint8_t evictedMac[6];
    setMac(evictedMac, 0x40, 0x40, 0x00);
    CHECK(smartcap_table_find(&T2, evictedMac) == NULL,
          "lowest-score entry was the eviction victim");

    // remove() keeps the count consistent.
    setMac(evictedMac, 0x40, 0x40, 0x01);
    CHECK(smartcap_table_remove(&T2, evictedMac), "remove() succeeds for a known bssid");
    CHECK(T2.count == SMCAP_MAX_AP - 1, "remove decrements count");
}

static void testFocus(void) {
    printf("focus / channel grouping:\n");
    smartcap_table_t T;
    smartcap_table_init(&T);
    smartcap_score_params_t p;
    smartcap_score_params_default(&p);

    // 6 targets on channels {1, 6, 6, 11, 11, 11}. focus_build recomputes each
    // score from the target fields: at rssi below floor every target starts
    // from base+novelty+recency (30) plus the RSSI term. The RSSI values below
    // yield recomputed scores {44,47,50,54,57,60}: the three channel-11
    // targets (54,57,60) score highest, so their channel must lead the focus.
    const int chOrder[] = {1, 6, 6, 11, 11, 11};
    const int rssiVals[] = {-67, -62, -57, -50, -45, -40};
    const uint32_t now = 2000;
    uint8_t bssid[6];
    for (int i = 0; i < 6; ++i) {
        setMac(bssid, (uint8_t)(0x60 + i), 0x60, 0x60);
        smartcap_target_t *e = smartcap_table_upsert(&T, bssid);
        e->channel = (uint8_t)chOrder[i];
        e->rssi = (int8_t)rssiVals[i];
        e->last_seen_ms = now; // fresh, so recency applies to all equally
    }

    smartcap_focus_t f;
    const uint8_t ndet = smartcap_focus_build(&T, &p, now, &f, nullptr, nullptr);
    CHECK(ndet == SMCAP_MAX_FOCUS && f.count == SMCAP_MAX_FOCUS,
          "focus caps at and reports SMCAP_MAX_FOCUS");

    // Top recomputed scores are the three channel-11 targets (60,57,54) plus
    // the strongest channel-6 one (50). So the first three entries are ch 11,
    // consecutive and score-descending within the channel.
    CHECK(f.entries[0].target->channel == 11 &&
          f.entries[1].target->channel == 11 &&
          f.entries[2].target->channel == 11,
          "highest-scoring channel leads the focus");
    CHECK(f.entries[0].target->score >= f.entries[1].target->score &&
          f.entries[1].target->score >= f.entries[2].target->score,
          "score desc within a channel");
}

static void testStrategy(void) {
    printf("strategy picker:\n");
    smartcap_target_t t;
    memset(&t, 0, sizeof(t));

    CHECK(smartcap_pick_strategy(&t) == SMCAP_STRATEGY_PASSIVE,
          "bare target -> passive");

    t.n_clients = 2;
    CHECK(smartcap_pick_strategy(&t) == SMCAP_STRATEGY_DEAUTH_CLIENT,
          "target with clients -> targeted deauth");

    t.n_clients = 0;
    t.flags = SMCAP_PMKID_METHOD;
    CHECK(smartcap_pick_strategy(&t) == SMCAP_STRATEGY_ASSOC_PMKID,
          "no clients + PMKID-capable -> silent assoc");

    t.n_clients = 1;
    CHECK(smartcap_pick_strategy(&t) == SMCAP_STRATEGY_DEAUTH_CLIENT,
          "clients always win over the silent method (they are the stronger signal)");
}

int main(void) {
    printf("SmartCap host test\n");
    testScore();
    testSmoothed();
    testTable();
    testFocus();
    testStrategy();
    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}