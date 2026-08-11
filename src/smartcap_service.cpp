// Firmware glue for the SmartCap subsystem.
//
// This file is the single place that owns the module instances and the ONE
// switch between observe-only and live mode. `live` comes straight from
// config.deauth_enabled:
//   - false: adapter stays in dry-run - the FSM observes and logs, nothing is
//     ever transmitted and the real radio channel is never touched;
//   - true:  the FSM owns channel hopping and targeted deauths. This is only
//     safe because the legacy rotateChannel()/performDeauthCycle() path that
//     fought for the same airtime has been retired (Stage D).
//
// The service also installs the two firmware hooks on the adapter:
//   - an RX gate that drops events for whitelisted BSSIDs (they can never make
//     it into the table, so they can never be scored or attacked);
//   - a notify sink that converts FSM channel/deauth actions back into the
//     PWN_EVENT_CHANNEL_CHANGED / PWN_EVENT_DEAUTH_SENT events the UI and the
//     JS plugins still consume.

#include "pentagotchi_internal.h"

#include "pentagotchi_app.h"
#include "pentagotchi_config.h"
#include "pentagotchi_events.h"
#include "smartcap_fsm.h"
#include "smartcap_radio.h"
#include "smartcap_result.h"
#include "smartcap_table.h"

using namespace pentagotchi::detail;

// ESP32 backend declared here (smartcap_radio_esp.cpp) - never linked on hosts.
extern const smartcap_radio_ops_t *smartcap_radio_esp_ops(void);

namespace pentagotchi::detail {

namespace {
smartcap_radio_t gSmartcapRadio;
smartcap_table_t gSmartcapTable;
smartcap_score_params_t gSmartcapScore;
smartcap_fsm_params_t gSmartcapParams;
smartcap_fsm_t gSmartcapFsm;
bool gSmartcapReady = false;

// NOTE: no locking around the ring. The adapter is a lock-free SPSC queue:
// one producer (WiFi task feed) and one consumer (loop task tick). Bear in
// mind the logged actions must never block (Serial is fine from a task, NOT
// from inside a critical section - that is what paniced the WDT before).

void radioLog(const char *line) { SERIAL_PRINTF("[smartcap] %s\n", line); }
void resultLog(const char *line) { SERIAL_PRINTF("[smartcap] %s\n", line); }

// RX gate: keep whitelisted BSSIDs out of the SmartCap table entirely. The
// gate is consulted per event while the wifi task feeds the ring, so it reads
// the current config (loaded before any frames flow).
bool serviceRadioGate(const smartcap_radio_event_t *ev, void *ctx) {
    (void)ctx;
    if (!gInstance) {
        return true;
    }
    const pentagotchi_config_t &cfg = gInstance->config();
    if (cfg.whitelist_count == 0) {
        return true;
    }
    for (uint8_t w = 0; w < cfg.whitelist_count; ++w) {
        if (memcmp(cfg.whitelist[w], ev->bssid, 6) == 0) {
            return false;
        }
    }
    return true;
}

// Notify sink: turn adapter actions into the pwn_events the UI/plugins listen
// to, preserving the legacy field layout (mac = AP, str = "XX:XX:..:XX").
void serviceRadioNotify(int type, int value, const uint8_t *mac, const char *str, void *ctx) {
    (void)ctx;
    switch (type) {
    case SMCAP_RADIO_NOTIFY_CHANNEL: {
        pwn_event_t ev = {};
        ev.value = value;
        pwn_events_raise(PWN_EVENT_CHANNEL_CHANGED, &ev);
        break;
    }
    case SMCAP_RADIO_NOTIFY_DEAUTH_SENT: {
        pwn_event_t ev = {};
        ev.mac = mac;
        ev.str = str;
        pwn_events_raise(PWN_EVENT_DEAUTH_SENT, &ev);
        break;
    }
    default:
        break;
    }
}

// FSM stage observer: the only stage the UI reflects is COOLDOWN (the pause
// between a failed attempt and the next), surfaced as PWN_EVENT_COOLDOWN with
// the pause duration so the UI can animate "sleeping" for exactly that long.
void serviceFsmCb(const smartcap_fsm_t *f, smartcap_stage_t stage, uint32_t param_ms, void *ctx) {
    (void)f;
    (void)ctx;
    if (stage != SMCAP_STAGE_COOLDOWN) {
        return;
    }
    pwn_event_t ev = {};
    ev.value = (int32_t)param_ms;
    pwn_events_raise(PWN_EVENT_COOLDOWN, &ev);
}
} // namespace

void smartcap_service_init(bool live) {
    if (gSmartcapReady) {
        return;
    }

    smartcap_radio_init(&gSmartcapRadio, smartcap_radio_esp_ops());
    smartcap_radio_set_logger(&gSmartcapRadio, radioLog);
    smartcap_radio_set_event_gate(&gSmartcapRadio, serviceRadioGate, nullptr);
    smartcap_radio_set_notify(&gSmartcapRadio, serviceRadioNotify, nullptr);
    smartcap_result_set_logger(resultLog);

    // The ONLY live/observe switch for the whole subsystem. The legacy
    // rotateChannel()/performDeauthCycle() path that used the same channels is
    // gone, so flipping this on cannot collide with another controller.
    smartcap_radio_set_dry_run(&gSmartcapRadio, !live);

    smartcap_table_init(&gSmartcapTable);
    smartcap_score_params_default(&gSmartcapScore);
    smartcap_fsm_params_default(&gSmartcapParams);
    smartcap_fsm_begin(&gSmartcapFsm, &gSmartcapRadio, &gSmartcapTable, &gSmartcapScore,
                      &gSmartcapParams, millis());
    smartcap_fsm_set_cb(&gSmartcapFsm, serviceFsmCb, nullptr);

    gSmartcapReady = true;
    SERIAL_PRINTF("[smartcap] service ready (live=%d, dry-run=%d, table=%d entries)\n",
                  live ? 1 : 0, smartcap_radio_is_dry_run(&gSmartcapRadio), SMCAP_MAX_AP);
}

void smartcap_service_feed_frame(const uint8_t *frame, uint16_t len, int8_t rssi,
                                 uint8_t channel) {
    if (!gSmartcapReady) {
        return;
    }
    smartcap_radio_report_frame(&gSmartcapRadio, frame, len, rssi, channel, millis());
}

void smartcap_service_tick(void) {
    if (!gSmartcapReady) {
        return;
    }
    smartcap_fsm_tick(&gSmartcapFsm, millis());
}

} // namespace pentagotchi::detail