#pragma once

// SmartCap radio adapter - the ONLY seam between the SmartCap FSM and the
// low-level Wi-Fi code (promiscuous RX callback, channel switching, frame
// injection). It holds zero decision logic: it translates
//   TX: FSM actions  -> optional low-level ops (or dry-run logging),
//   RX: raw frames   -> small normalized events the FSM can consume.
//
// SAFETY SWITCH
// -------------
// The adapter defaults to dry-run mode (SMCAP_RADIO_DEFAULT_DRY_RUN = 1):
// every "sending" action (deauth, assoc) is logged with full context and is
// NEVER transmitted; channel changes only update a *virtual* channel so the
// FSM can be exercised without touching the real radio.
//
//   AS LONG AS THIS FLAG STAYS 1 THE FSM CANNOT CHANGE AIRTIME AT ALL.
//
// Broadcast deauth is additionally gated behind allow_broadcast, which is 0 by
// default and independent of dry-run: even in live mode it cannot fire unless
// explicitly enabled (it must stay physically unreachable for the "aggressive"
// reserve strategy).

#include "smartcap_types.h"

#include <stdbool.h>
#include <stdint.h>

#define SMCAP_RADIO_DEFAULT_DRY_RUN 1 // 1 = observe & log only, never transmit
#define SMCAP_RADIO_QUEUE            32
#define SMCAP_RADIO_LOGLINE          160

// ---------------------------------------------------------------------------
// Low-level backend, provided by the firmware (esp) or a mock in host tests.
// Callable only when the adapter is out of dry-run; otherwise unused.
// ---------------------------------------------------------------------------
typedef struct {
    bool (*get_channel)(uint8_t *out);
    bool (*set_channel)(uint8_t channel);
    void (*deauth_client)(const uint8_t *ap, const uint8_t *client);
    void (*deauth_bcast)(const uint8_t *ap);
    void (*assoc_pmkid)(const uint8_t *ap, const char *ssid); // inject assoc req asking for a PMKID
} smartcap_radio_ops_t;

// ---------------------------------------------------------------------------
// Normalized inbound events pushed by the adapter's frame parser (or directly
// by the firmware when a higher-level signal is available, e.g. PMKID parse).
// ---------------------------------------------------------------------------
typedef enum {
    SMCAP_EV_NONE = 0,
    SMCAP_EV_AP_SEEN,     // bssid/ssid observed, fields: ssid, bssid, rssi, channel
    SMCAP_EV_CLIENT_SEEN, // station talked to a bssid: mac, bssid, rssi, channel
    SMCAP_EV_EAPOL_SEEN,  // a 4-way message was heard: bssid, msg (1..4)
    SMCAP_EV_PMKID_SEEN,  // PMKID extracted from an assoc response: bssid
} smartcap_radio_event_type_t;

typedef struct {
    smartcap_radio_event_type_t type;
    uint32_t ts_ms;
    int16_t rssi;
    uint8_t channel;
    uint8_t msg; // EAPOL 1..4
    bool pmkid_method; // AP_SEEN: beacon carries an RSN IE -> silent assoc/PMKID viable
    uint8_t bssid[6];
    uint8_t mac[6]; // client MAC (CLIENT_SEEN) or same as bssid otherwise
    char ssid[SMCAP_SSID_MAX];
} smartcap_radio_event_t;

typedef void (*smartcap_log_fn)(const char *line);

// Optional firmware-side observation hooks (see smartcap_radio_set_notify /
// smartcap_radio_set_event_gate). They carry NO decision power - the adapter
// stays the single seam for TX/RX - but let the firmware turn FSM actions into
// the high-level events the UI/plugins already consume.
typedef enum {
    SMCAP_RADIO_NOTIFY_NONE = 0,
    SMCAP_RADIO_NOTIFY_CHANNEL,     // radio (or virtual) channel moved; value = new channel
    SMCAP_RADIO_NOTIFY_DEAUTH_SENT, // deauth frame really transmitted; mac/str = AP
} smartcap_radio_notify_type_t;

typedef void (*smartcap_radio_notify_fn)(int type, int value, const uint8_t *mac,
                                         const char *str, void *ctx);

// RX filter installed before an event enters the queue. Return false to drop
// the event entirely (e.g. firmware whitelist: never track/attack those BSSIDs).
typedef bool (*smartcap_radio_event_gate_fn)(const smartcap_radio_event_t *ev, void *ctx);

typedef struct {
    volatile uint32_t prod; // writer index, incremented by feed/parser side
    volatile uint32_t cons; // reader index, incremented by poll() side
    smartcap_radio_event_t queue[SMCAP_RADIO_QUEUE];

    const smartcap_radio_ops_t *ops;
    smartcap_log_fn logger;

    smartcap_radio_notify_fn notify;        // firmware event emitter (CHANNEL/DEAUTH_SENT)
    void *notify_ctx;
    smartcap_radio_event_gate_fn event_gate; // RX filter (firmware whitelist)
    void *event_gate_ctx;

    bool dry_run;         // SMCAP_RADIO_DEFAULT_DRY_RUN unless changed
    bool allow_broadcast; // independent gate: broadcast deauth never fires while 0
    uint8_t virt_channel; // FSM's view of the current channel in dry-run
    uint8_t selected_channel; // last channel handed to set_channel() (notify dedup)

    // Execution counters: incremented ONLY when an action really went to the
    // backend (i.e. never in dry-run). Lets host tests distinguish dry-run
    // logging from real transmission.
    uint32_t n_set_channel;
    uint32_t n_deauth_client;
    uint32_t n_deauth_bcast;
    uint32_t n_assoc_pmkid;
} smartcap_radio_t;

void smartcap_radio_init(smartcap_radio_t *r, const smartcap_radio_ops_t *ops);
void smartcap_radio_set_logger(smartcap_radio_t *r, smartcap_log_fn fn);

// Optional observation hooks (never touched unless installed by the firmware):
void smartcap_radio_set_notify(smartcap_radio_t *r, smartcap_radio_notify_fn fn, void *ctx);
void smartcap_radio_set_event_gate(smartcap_radio_t *r, smartcap_radio_event_gate_fn fn, void *ctx);

// The two knobs the safety review must check before anything goes live:
void smartcap_radio_set_dry_run(smartcap_radio_t *r, bool on);          // default: true
void smartcap_radio_set_allow_broadcast(smartcap_radio_t *r, bool on);  // default: false
bool smartcap_radio_is_dry_run(const smartcap_radio_t *r);

// RX: raw frame in (FCS included, as handed by the Wi-Fi driver) -> normalized
// event pushed into the SPSC queue. Returns true when accepted.
bool smartcap_radio_report_frame(smartcap_radio_t *r, const uint8_t *frame,
                                 uint16_t len, int8_t rssi, uint8_t channel,
                                 uint32_t now_ms);

// RX: enqueue a pre-built event (used internally and by firmware extensions).
bool smartcap_radio_push_event(smartcap_radio_t *r, const smartcap_radio_event_t *ev);

// RX: pull the next pending event; false when the queue is empty.
bool smartcap_radio_poll(smartcap_radio_t *r, smartcap_radio_event_t *out);

// TX (all gated by dry-run):
bool smartcap_radio_get_channel(smartcap_radio_t *r, uint8_t *out);
bool smartcap_radio_set_channel(smartcap_radio_t *r, uint8_t channel);
void smartcap_radio_deauth_client(smartcap_radio_t *r, const uint8_t *ap, const uint8_t *client);
void smartcap_radio_deauth_bcast(smartcap_radio_t *r, const uint8_t *ap); // + allow_broadcast
void smartcap_radio_assoc_pmkid(smartcap_radio_t *r, const uint8_t *ap, const char *ssid);

void smartcap_radio_counters(const smartcap_radio_t *r, uint32_t out[4]); // [set_ch,deauth_c,deauth_b,assoc]