// Host tests for the SmartCap radio adapter + stage FSM. Uses a mock radio
// backend so nothing touches a real Wi-Fi device:
//   - adapter: dry-run gating, broadcast gate, frame -> event parsing,
//   - FSM: stage transitions, empty-focus handling, LISTEN timeout/cooldown,
//     multi-target channel-group ordering, passive captures, irregular ticks.
//
// Run with tools/run_smartcap_fsm_test.sh

#include "smartcap_attack.h"
#include "smartcap_fsm.h"
#include "smartcap_radio.h"
#include "smartcap_result.h"
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
    out[3] = 0x66; out[4] = 0x77; out[5] = 0x88;
}

// ---------------------------------------------------------------------------
// mock radio backend
// ---------------------------------------------------------------------------
static uint8_t gCurChan = 1;
static int gNSet = 0;
static int gNDeauthClient = 0;
static int gNDeauthBcast = 0;
static int gNAssoc = 0;
static uint8_t gLastClient[6] = {0};
static uint8_t gLastAp[6] = {0};
static char gLastAssocSsid[64];

static bool mockGetChannel(uint8_t *out) { *out = gCurChan; return true; }
static bool mockSetChannel(uint8_t ch) { gCurChan = ch; ++gNSet; return true; }
static void mockDeauthClient(const uint8_t *ap, const uint8_t *client) {
    memcpy(gLastAp, ap, 6); memcpy(gLastClient, client, 6); ++gNDeauthClient;
}
static void mockDeauthBcast(const uint8_t *ap) { memcpy(gLastAp, ap, 6); ++gNDeauthBcast; }
static void mockAssoc(const uint8_t *ap, const char *ssid) {
    memcpy(gLastAp, ap, 6);
    if (ssid) { snprintf(gLastAssocSsid, sizeof(gLastAssocSsid), "%s", ssid); }
    else { gLastAssocSsid[0] = '\0'; }
    ++gNAssoc;
}

static const smartcap_radio_ops_t gOps = {
    mockGetChannel, mockSetChannel, mockDeauthClient, mockDeauthBcast, mockAssoc,
};

// ---------------------------------------------------------------------------
// logger capture
// ---------------------------------------------------------------------------
static char gLog[8192];
static size_t gLogLen;

static void captureLog(const char *line) {
    const size_t l = strlen(line);
    if (gLogLen + l + 2 < sizeof(gLog)) {
        memcpy(gLog + gLogLen, line, l);
        gLogLen += l;
        gLog[gLogLen++] = '\n';
        gLog[gLogLen] = '\0';
    }
}

static void resetLog(void) { gLog[0] = '\0'; gLogLen = 0; }

static int occurrences(const char *needle) {
    int n = 0;
    const char *p = gLog;
    size_t l = strlen(needle);
    while ((p = strstr(p, needle)) != nullptr) {
        ++n;
        p += l;
    }
    return n;
}

static void focusLogHook(void *ctx, const char *line) {
    (void)ctx;
    captureLog(line);
}

// ---------------------------------------------------------------------------
// result capture sink
// ---------------------------------------------------------------------------
static uint8_t gCaptured[8][6];
static int gNCaptured;
static int gNHs;
static int gNPm;

static void resetCapture(void) { gNCaptured = gNHs = gNPm = 0; }

static void hsHook(const smartcap_target_t *t, uint32_t now) {
    (void)now;
    memcpy(gCaptured[gNCaptured++], t->bssid, 6);
    ++gNHs;
}
static void pmHook(const smartcap_target_t *t, uint32_t now) {
    (void)now;
    memcpy(gCaptured[gNCaptured++], t->bssid, 6);
    ++gNPm;
}

// ---------------------------------------------------------------------------
// event helpers
// ---------------------------------------------------------------------------
static void pushAp(smartcap_radio_t *r, const uint8_t *bssid, uint8_t ch, int8_t rssi,
                   const char *ssid, uint32_t now) {
    smartcap_radio_event_t ev = {};
    ev.type = SMCAP_EV_AP_SEEN;
    ev.ts_ms = now;
    ev.rssi = rssi;
    ev.channel = ch;
    memcpy(ev.bssid, bssid, 6);
    memcpy(ev.mac, bssid, 6);
    if (ssid) snprintf(ev.ssid, sizeof(ev.ssid), "%s", ssid);
    smartcap_radio_push_event(r, &ev);
}

static void pushClient(smartcap_radio_t *r, const uint8_t *bssid, uint8_t ch, int8_t rssi,
                       const uint8_t *mac, uint32_t now) {
    smartcap_radio_event_t ev = {};
    ev.type = SMCAP_EV_CLIENT_SEEN;
    ev.ts_ms = now;
    ev.rssi = rssi;
    ev.channel = ch;
    memcpy(ev.bssid, bssid, 6);
    memcpy(ev.mac, mac, 6);
    smartcap_radio_push_event(r, &ev);
}

static void pushEapol(smartcap_radio_t *r, const uint8_t *bssid, uint8_t msg, uint32_t now) {
    smartcap_radio_event_t ev = {};
    ev.type = SMCAP_EV_EAPOL_SEEN;
    ev.ts_ms = now;
    ev.msg = msg;
    memcpy(ev.bssid, bssid, 6);
    memcpy(ev.mac, bssid, 6);
    smartcap_radio_push_event(r, &ev);
}

static void pushPmkid(smartcap_radio_t *r, const uint8_t *bssid, uint32_t now) {
    smartcap_radio_event_t ev = {};
    ev.type = SMCAP_EV_PMKID_SEEN;
    ev.ts_ms = now;
    memcpy(ev.bssid, bssid, 6);
    memcpy(ev.mac, bssid, 6);
    smartcap_radio_push_event(r, &ev);
}

// ---------------------------------------------------------------------------
// raw-frame builders for the parser tests
// ---------------------------------------------------------------------------
static void buildBeacon(uint8_t *f, const uint8_t *bssid, const char *ssid, size_t *lenOut) {
    memset(f, 0, 128);
    f[0] = 0x80; f[1] = 0x00;             // mgmt beacon
    for (int i = 4; i < 10; ++i) f[i] = 0xFF; // bcast da
    memcpy(f + 10, bssid, 6);
    memcpy(f + 16, bssid, 6);
    f[32] = 0x64; f[33] = 0x00;           // beacon interval
    const size_t ss = strlen(ssid);
    f[36] = 0; f[37] = (uint8_t)ss;       // SSID IE
    memcpy(f + 38, ssid, ss);
    *lenOut = 36 + 2 + ss + 4;            // +4 trailing FCS the driver includes
}

static void buildDataToDs(uint8_t *f, const uint8_t *bssid, const uint8_t *client, size_t *lenOut) {
    memset(f, 0, 64);
    f[0] = 0x08; f[1] = 0x01;             // data, to DS
    memcpy(f + 4, bssid, 6);  // addr1 = dest bssid
    memcpy(f + 10, client, 6); // addr2 = station
    memcpy(f + 16, bssid, 6); // addr3 = bssid
    *lenOut = 24 + 4;                     // +4 trailing FCS

}

static void buildDataFromDs(uint8_t *f, const uint8_t *bssid, const uint8_t *client, size_t *lenOut) {
    memset(f, 0, 64);
    f[0] = 0x08; f[1] = 0x02;             // data, from DS
    memcpy(f + 4, client, 6);  // addr1 = station
    memcpy(f + 10, bssid, 6);  // addr2 = bssid (AP)
    memcpy(f + 16, bssid, 6);  // addr3 = bssid
    *lenOut = 24 + 4;                     // +4 trailing FCS
}

// RSN IE payload helper shared by the PMKID builders. When `withPmkid` the IE
// appends a single PMKID-list entry (what an AP's assoc response carries).
static size_t writeRsnIe(uint8_t *out, bool pskAkm, bool pmkidCap, bool withPmkid) {
    uint8_t body[64];
    size_t p = 0;
    body[p++] = 0x01; body[p++] = 0x00;             // version
    body[p++] = 0x00; body[p++] = 0x0f; body[p++] = 0xac; body[p++] = 0x04; // group CCMP
    body[p++] = 0x01; body[p++] = 0x00;             // pairwise count
    body[p++] = 0x00; body[p++] = 0x0f; body[p++] = 0xac; body[p++] = 0x04; // pairwise CCMP
    body[p++] = 0x01; body[p++] = 0x00;             // akm count
    body[p++] = 0x00; body[p++] = 0x0f; body[p++] = 0xac;
    body[p++] = pskAkm ? 0x02 : 0x01;               // akm PSK / (else) 802.1X
    body[p++] = pmkidCap ? 0x80 : 0x00; body[p++] = 0x00; // rsn capabilities
    if (withPmkid) {
        body[p++] = 0x01; body[p++] = 0x00;         // pmkid count
        for (int i = 0; i < 16; ++i) body[p++] = 0xBA; // dummy pmkid
    }
    out[0] = 48;                                    // IE tag: RSN
    out[1] = (uint8_t)p;
    memcpy(out + 2, body, p);
    return p + 2;
}

// beacon carrying an RSN IE after the SSID IE (WPA2-style network).
static void buildBeaconRsn(uint8_t *f, const uint8_t *bssid, const char *ssid,
                           bool pmkidCap, size_t *lenOut) {
    memset(f, 0, 128);
    f[0] = 0x80; f[1] = 0x00;
    for (int i = 4; i < 10; ++i) f[i] = 0xFF;
    memcpy(f + 10, bssid, 6);
    memcpy(f + 16, bssid, 6);
    f[32] = 0x64; f[33] = 0x00;
    const size_t ss = strlen(ssid);
    f[36] = 0; f[37] = (uint8_t)ss;
    memcpy(f + 38, ssid, ss);
    size_t pos = 38 + ss;
    pos += writeRsnIe(f + pos, true, pmkidCap, false);
    *lenOut = pos + 4;
}

// association response (subtype 1) from `bssid` to `station`; optionally
// carrying a PMKID list in its RSN IE.
static void buildAssocResponse(uint8_t *f, const uint8_t *bssid, const uint8_t *station,
                               bool withPmkid, size_t *lenOut) {
    memset(f, 0, 128);
    f[0] = 0x10; f[1] = 0x00;             // mgmt assoc response
    memcpy(f + 4, station, 6);            // DA = the station
    memcpy(f + 10, bssid, 6);             // SA = bssid (AP)
    memcpy(f + 16, bssid, 6);             // BSSID = AP
    f[24] = 0x01; f[25] = 0x00;           // capability: ESS
    f[26] = 0x00; f[27] = 0x00;           // status: success
    f[28] = 0x01; f[29] = 0xC0;           // AID
    size_t pos = 30;
    pos += writeRsnIe(f + pos, true, false, withPmkid);
    *lenOut = pos + 4;
}

static void buildQosEapolM1(uint8_t *f, const uint8_t *bssid, const uint8_t *station, size_t *lenOut) {
    memset(f, 0, 96);
    f[0] = 0x88; f[1] = 0x01;             // QoS data, to DS (header = 26 bytes)
    memcpy(f + 4, bssid, 6);    // addr1 = dest AP
    memcpy(f + 10, station, 6); // addr2 = station
    memcpy(f + 16, bssid, 6);   // addr3 = bssid
    // QoS control at 24..25, LLC at 26
    static const uint8_t llc[8] = {0xAA, 0xAA, 0x03, 0, 0, 0, 0x88, 0x8E};
    memcpy(f + 26, llc, 8);
    f[34] = 2; f[35] = 3;                 // EAPOL ver/type
    f[38] = 2;                             // 802.11 descriptor version
    f[39] = 0x00; f[40] = 0x80;            // Key Info: ACK set => Message 1
    *lenOut = 96;
}

// ---------------------------------------------------------------------------
// tests
// ---------------------------------------------------------------------------
static void testAdapterGating(void) {
    printf("adapter dry-run / broadcast gating:\n");
    smartcap_radio_t r;
    smartcap_radio_init(&r, &gOps);
    smartcap_radio_set_logger(&r, captureLog);
    resetLog();

    uint8_t ap[6], cli[6];
    setMac(ap, 0xaa, 0xaa, 0xaa);
    setMac(cli, 0xbb, 0xbb, 0xbb);

    CHECK(smartcap_radio_is_dry_run(&r), "adapter defaults to dry-run");

    uint32_t ctr[4];
    smartcap_radio_counters(&r, ctr);
    CHECK(ctr[0] == 0 && ctr[1] == 0 && ctr[2] == 0 && ctr[3] == 0, "counters start at 0");

    smartcap_radio_set_channel(&r, 6);
    uint8_t ch = 0;
    smartcap_radio_get_channel(&r, &ch);
    CHECK(ch == 6, "dry-run set_channel updates the virtual channel");
    smartcap_radio_counters(&r, ctr);
    CHECK(ctr[0] == 0, "dry-run set_channel never touches the backend");
    CHECK(occurrences("set_channel(6)") == 1, "dry-run set_channel is logged");

    smartcap_radio_deauth_client(&r, ap, cli);
    smartcap_radio_counters(&r, ctr);
    CHECK(ctr[1] == 0, "dry-run deauth_client is not transmitted");
    CHECK(occurrences("deauth_client") == 1, "dry-run deauth_client is logged");

    smartcap_radio_deauth_bcast(&r, ap);
    smartcap_radio_counters(&r, ctr);
    CHECK(ctr[2] == 0, "broadcast blocked in dry-run too");
    CHECK(occurrences("BLOCKED") == 1, "blocked broadcast is reported");

    smartcap_radio_set_allow_broadcast(&r, true);
    smartcap_radio_deauth_bcast(&r, ap);
    smartcap_radio_counters(&r, ctr);
    CHECK(ctr[2] == 0, "broadcast still not transmitted while dry-run");
    CHECK(occurrences("deauth_bcast") == 1, "broadcast would-send is logged in dry-run");

    // live mode
    smartcap_radio_set_dry_run(&r, false);
    smartcap_radio_set_channel(&r, 6);
    smartcap_radio_deauth_client(&r, ap, cli);
    smartcap_radio_deauth_bcast(&r, ap); // allow_broadcast still true
    smartcap_radio_get_channel(&r, &ch);
    CHECK(ch == 6, "live get_channel reads the backend");
    smartcap_radio_counters(&r, ctr);
    CHECK(ctr[0] == 1 && ctr[1] == 1 && ctr[2] == 1, "live actions reach the backend");
    CHECK(gNDeauthClient == 1 && gNDeauthBcast == 1, "mock saw the live frames");

    // broadcast gate is INDEPENDENT of dry-run: live + disallowed -> blocked
    smartcap_radio_set_allow_broadcast(&r, false);
    resetLog();
    smartcap_radio_deauth_bcast(&r, ap);
    smartcap_radio_counters(&r, ctr);
    CHECK(ctr[2] == 1, "blocked broadcast is not counted even in live mode");
    CHECK(occurrences("BLOCKED") == 1, "live + disallowed broadcast is blocked");
}

static void testAdapterParsing(void) {
    printf("adapter frame parsing:\n");
    smartcap_radio_t r;
    smartcap_radio_init(&r, nullptr); // no backend needed, RX only
    smartcap_radio_event_t ev;

    uint8_t bssid[6];
    setMac(bssid, 0x12, 0x34, 0x56);

    uint8_t frame[128];
    size_t len = 0;
    buildBeacon(frame, bssid, "MyNet", &len);
    CHECK(smartcap_radio_report_frame(&r, frame, (uint16_t)len, -50, 6, 1000), "beacon accepted");
    CHECK(smartcap_radio_poll(&r, &ev), "beacon yields an event");
    CHECK(ev.type == SMCAP_EV_AP_SEEN, "beacon -> AP_SEEN");
    CHECK(memcmp(ev.bssid, bssid, 6) == 0, "AP_SEEN carries the BSSID");
    CHECK(strcmp(ev.ssid, "MyNet") == 0, "AP_SEEN carries the SSID");
    CHECK(ev.channel == 6 && ev.rssi == -50, "AP_SEEN carries channel + rssi");

    uint8_t client[6];
    setMac(client, 0xaa, 0xcd, 0xef);
    buildDataToDs(frame, bssid, client, &len);
    CHECK(smartcap_radio_report_frame(&r, frame, (uint16_t)len, -54, 6, 1100), "data accepted");
    CHECK(smartcap_radio_poll(&r, &ev), "data yields an event");
    CHECK(ev.type == SMCAP_EV_CLIENT_SEEN, "toDS data -> CLIENT_SEEN");
    CHECK(memcmp(ev.bssid, bssid, 6) == 0, "CLIENT_SEEN maps to the correct AP");
    CHECK(memcmp(ev.mac, client, 6) == 0, "CLIENT_SEEN carries the station MAC");

    uint8_t station[6];
    setMac(station, 0x77, 0x88, 0x99);
    buildQosEapolM1(frame, bssid, station, &len);
    CHECK(smartcap_radio_report_frame(&r, frame, (uint16_t)len, -52, 6, 1200), "eapol accepted");
    CHECK(smartcap_radio_poll(&r, &ev), "eapol yields an event");
    CHECK(ev.type == SMCAP_EV_EAPOL_SEEN && ev.msg == 1, "eapol M1 -> EAPOL_SEEN msg=1");
    CHECK(memcmp(ev.bssid, bssid, 6) == 0, "EAPOL_SEEN maps to the AP");

    // fromDS data frames carry the station in addr1, AP in addr3.
    buildDataFromDs(frame, bssid, client, &len);
    CHECK(smartcap_radio_report_frame(&r, frame, (uint16_t)len, -54, 6, 1300), "fromDS datagram accepted");
    CHECK(smartcap_radio_poll(&r, &ev), "fromDS yields an event");
    CHECK(ev.type == SMCAP_EV_CLIENT_SEEN, "fromDS data -> CLIENT_SEEN");
    CHECK(memcmp(ev.bssid, bssid, 6) == 0, "fromDS maps to the AP (addr3)");
    CHECK(memcmp(ev.mac, client, 6) == 0, "fromDS client is addr1, not the AP");

    // DS bits equal (ad-hoc / WDS / management payload): no clear station.
    memset(frame, 0, 64);
    frame[0] = 0x08; frame[1] = 0x00;
    memcpy(frame + 4, bssid, 6);
    memcpy(frame + 10, client, 6);
    memcpy(frame + 16, bssid, 6);
    CHECK(!smartcap_radio_report_frame(&r, frame, 28, -55, 6, 1400),
          "DS-bits-equal data frame is rejected (no unambiguous client)");

    CHECK(!smartcap_radio_poll(&r, &ev), "queue drains empty");
}

static void testAdapterPmkidRsn(void) {
    printf("adapter PMKID RX (RSN hint + assoc-response):\n");
    smartcap_radio_t r;
    smartcap_radio_init(&r, nullptr); // RX only
    smartcap_radio_event_t ev;

    uint8_t bssid[6];
    setMac(bssid, 0x21, 0x21, 0x21);

    // Beacon without an RSN IE: no PMKID hint.
    uint8_t frame[128];
    size_t len = 0;
    buildBeacon(frame, bssid, "Plain", &len);
    CHECK(smartcap_radio_report_frame(&r, frame, (uint16_t)len, -50, 6, 1000),
          "plain beacon accepted");
    CHECK(smartcap_radio_poll(&r, &ev) && ev.type == SMCAP_EV_AP_SEEN && !ev.pmkid_method,
          "plain beacon: no PMKID hint");

    // Beacon with an RSN IE: the silent assoc method is viable.
    buildBeaconRsn(frame, bssid, "WPA", true, &len);
    CHECK(smartcap_radio_report_frame(&r, frame, (uint16_t)len, -50, 6, 1100),
          "RSN beacon accepted");
    CHECK(smartcap_radio_poll(&r, &ev), "RSN beacon yields an event");
    CHECK(ev.type == SMCAP_EV_AP_SEEN && ev.pmkid_method, "RSN beacon: PMKID hint set");
    CHECK(strcmp(ev.ssid, "WPA") == 0, "RSN beacon still carries the SSID");

    // Association response with a PMKID list -> a real capture.
    uint8_t station[6];
    setMac(station, 0x22, 0x22, 0x22);
    buildAssocResponse(frame, bssid, station, true, &len);
    CHECK(smartcap_radio_report_frame(&r, frame, (uint16_t)len, -50, 6, 1200),
          "RSN assoc-response accepted");
    CHECK(smartcap_radio_poll(&r, &ev), "assoc-response yields an event");
    CHECK(ev.type == SMCAP_EV_PMKID_SEEN, "PMKID list -> PMKID_SEEN");
    CHECK(memcmp(ev.bssid, bssid, 6) == 0, "PMKID_SEEN maps to the AP (SA)");

    // Association response WITHOUT a PMKID list -> ignored.
    buildAssocResponse(frame, bssid, station, false, &len);
    CHECK(!smartcap_radio_report_frame(&r, frame, (uint16_t)len, -50, 6, 1300),
          "assoc-response without PMKID is ignored");
    CHECK(!smartcap_radio_poll(&r, &ev), "no event left in the queue");
}

// The strategy flag is observational: a beacon's RSN IE marks a target as
// PMKID-method capable, and in live mode the injected assoc request must reach
// the backend with the AP and the SSID it uses for its claim. Defined after
// makeEmptyFsm below.
static void testFsPmkidFlagFromBeacon(void);

// ---------------------------------------------------------------------------
// notify + RX gate hooks
// ---------------------------------------------------------------------------
static int gNotifyType = 0;
static int gNotifyValue = -1;
static uint8_t gNotifyMac[6] = {0};
static char gNotifyStr[32];

static void captureNotify(int type, int value, const uint8_t *mac, const char *str, void *ctx) {
    (void)ctx;
    gNotifyType = type;
    gNotifyValue = value;
    if (mac) memcpy(gNotifyMac, mac, 6);
    if (str) snprintf(gNotifyStr, sizeof(gNotifyStr), "%s", str);
    else     gNotifyStr[0] = '\0';
}

static void testRadioNotifyGate(void) {
    printf("adapter notify hooks + RX event gate:\n");

    // Channel notify: fires on a distinct change (dry-run: a virtual hop),
    // never on repeats.
    smartcap_radio_t r;
    smartcap_radio_init(&r, &gOps);
    smartcap_radio_set_notify(&r, captureNotify, (void *)0x1234);
    gNotifyType = 0; gNotifyValue = -1;
    smartcap_radio_set_channel(&r, 6);
    CHECK(gNotifyType == SMCAP_RADIO_NOTIFY_CHANNEL && gNotifyValue == 6,
          "channel change fires the CHANNEL notify");
    gNotifyType = 0;
    smartcap_radio_set_channel(&r, 6);
    CHECK(gNotifyType == 0, "re-selecting the same channel does not re-notify");
    smartcap_radio_set_channel(&r, 11);
    CHECK(gNotifyType == SMCAP_RADIO_NOTIFY_CHANNEL && gNotifyValue == 11,
          "a distinct channel notifies again");

    // Deauth notify: only when a frame is really transmitted - dry-run keeps
    // the observer silent (no pretend attacks for UI/plugins).
    uint8_t ap[6], cli[6];
    setMac(ap, 0x91, 0x91, 0x91);
    setMac(cli, 0x92, 0x92, 0x92);
    gNotifyType = 0;
    smartcap_radio_deauth_client(&r, ap, cli);
    CHECK(gNotifyType == 0, "dry-run deauth reports nothing (observe-only)");

    smartcap_radio_set_dry_run(&r, false);
    gNotifyType = 0;
    smartcap_radio_deauth_client(&r, ap, cli);
    CHECK(gNotifyType == SMCAP_RADIO_NOTIFY_DEAUTH_SENT, "live deauth fires DEAUTH_SENT");
    CHECK(memcmp(gNotifyMac, ap, 6) == 0, "DEAUTH_SENT carries the AP MAC");
    CHECK(strcmp(gNotifyStr, "91:91:91:66:77:88") == 0, "DEAUTH_SENT carries the AP MAC string");

    smartcap_radio_set_allow_broadcast(&r, true);
    gNotifyType = 0;
    smartcap_radio_deauth_bcast(&r, ap);
    CHECK(gNotifyType == SMCAP_RADIO_NOTIFY_DEAUTH_SENT, "live broadcast deauth notifies too");

    // RX gate: rejected BSSIDs never enter the queue (firmware whitelist).
    smartcap_radio_t r2;
    smartcap_radio_init(&r2, nullptr); // RX only
    smartcap_radio_event_t ev;
    uint8_t reject[6], allow[6];
    setMac(reject, 0x51, 0x51, 0x51);
    setMac(allow, 0x52, 0x52, 0x52);

    auto gate = [](const smartcap_radio_event_t *e, void *ctx) {
        static const uint8_t banned[6] = {0x51, 0x51, 0x51, 0x66, 0x77, 0x88};
        (void)ctx;
        return memcmp(e->bssid, banned, 6) != 0;
    };
    smartcap_radio_set_event_gate(&r2, gate, nullptr);

    uint8_t frame[128];
    size_t len = 0;
    buildBeacon(frame, allow, "Allowed", &len);
    CHECK(smartcap_radio_report_frame(&r2, frame, (uint16_t)len, -50, 6, 2000),
          "non-banned beacon accepted");
    CHECK(smartcap_radio_poll(&r2, &ev) && ev.type == SMCAP_EV_AP_SEEN,
          "non-banned beacon yields an event");

    buildBeacon(frame, reject, "Banned", &len);
    CHECK(!smartcap_radio_report_frame(&r2, frame, (uint16_t)len, -50, 6, 2100),
          "banned beacon rejected by the gate");
    CHECK(!smartcap_radio_poll(&r2, &ev), "banned BSSID never reaches the queue");
}

static smartcap_fsm_t makeEmptyFsm(smartcap_radio_t *r, smartcap_table_t *t,
                                   smartcap_score_params_t *sp, smartcap_fsm_params_t *fp) {
    smartcap_fsm_t f;
    smartcap_fsm_begin(&f, r, t, sp, fp, 0);
    return f;
}

static void testFsEmptyFocusHunts(void) {
    printf("fsm: empty focus never attacks:\n");
    smartcap_radio_t r;
    smartcap_radio_init(&r, &gOps);
    smartcap_radio_set_logger(&r, captureLog);
    smartcap_table_t t;
    smartcap_table_init(&t);
    smartcap_score_params_t sp;
    smartcap_score_params_default(&sp);
    smartcap_fsm_params_t fp;
    smartcap_fsm_params_default(&fp);
    smartcap_fsm_t f = makeEmptyFsm(&r, &t, &sp, &fp);

    resetLog();
    bool sawScan = false;
    const uint32_t rescore0 = f.rescore_next_ms;
    for (uint32_t now = 0; now <= 60000;) {
        smartcap_fsm_tick(&f, now);
        const smartcap_stage_t s = smartcap_fsm_stage(&f);
        if (s == SMCAP_STAGE_SCAN) sawScan = true;
        CHECK(s != SMCAP_STAGE_ATTACK && s != SMCAP_STAGE_LISTEN && s != SMCAP_STAGE_COOLDOWN,
              "empty table must never reach an attack stage");
        now += 500;
    }
    CHECK(sawScan, "empty table idles in SCAN");
    CHECK(f.rescore_next_ms > rescore0,
          "RESCORE ran at least once (priority timer advanced)");
}

static void testFsExternalRescore(void) {
    printf("fsm: external rescore request breaks SCAN early:\n");
    smartcap_radio_t r;
    smartcap_radio_init(&r, &gOps);
    smartcap_radio_set_logger(&r, captureLog);
    smartcap_table_t t;
    smartcap_table_init(&t);
    smartcap_score_params_t sp;
    smartcap_score_params_default(&sp);
    smartcap_fsm_params_t fp;
    smartcap_fsm_params_default(&fp);
    smartcap_fsm_t f = makeEmptyFsm(&r, &t, &sp, &fp);

    // rescore_next is at 5000; force it before the timer.
    smartcap_fsm_rescore(&f);
    smartcap_fsm_tick(&f, 500);
    CHECK(f.rescore_next_ms == 500 + fp.rescore_period_ms, "rescore ran on demand (timer moved)");
    CHECK(smartcap_fsm_stage(&f) == SMCAP_STAGE_SCAN, "empty focus lands back in SCAN");
}

static void testFsMultiTargetGroup(void) {
    printf("fsm: multiple same-channel targets processed in score order:\n");
    smartcap_radio_t r;
    smartcap_radio_init(&r, &gOps);
    smartcap_radio_set_logger(&r, captureLog);
    smartcap_table_t t;
    smartcap_table_init(&t);
    smartcap_score_params_t sp;
    smartcap_score_params_default(&sp);
    smartcap_fsm_params_t fp;
    smartcap_fsm_params_default(&fp);

    uint8_t apA[6], apB[6], apC[6];
    setMac(apA, 0x01, 0x01, 0x01);
    setMac(apB, 0x02, 0x02, 0x02);
    setMac(apC, 0x03, 0x03, 0x03);
    uint8_t clA[6], clB[6], clC[6];
    setMac(clA, 0x1a, 0x1a, 0x1a);
    setMac(clB, 0x1c, 0x1c, 0x1c);
    setMac(clC, 0x1e, 0x1e, 0x1e);

    smartcap_result_set_handshake_fn(hsHook);
    smartcap_result_set_pmkid_fn(pmHook);
    resetCapture();

    // Strongest RSSI first => score order A > B > C, all on channel 6.
    pushAp(&r, apA, 6, -45, "A", 0);
    pushAp(&r, apB, 6, -60, "B", 0);
    pushAp(&r, apC, 6, -75, "C", 0);
    pushClient(&r, apA, 6, -45, clA, 0);
    pushClient(&r, apB, 6, -60, clB, 0);
    pushClient(&r, apC, 6, -75, clC, 0);

    smartcap_fsm_t f = makeEmptyFsm(&r, &t, &sp, &fp);
    resetLog();

    smartcap_fsm_tick(&f, 0);
    smartcap_fsm_tick(&f, 1000);
    smartcap_fsm_tick(&f, 2000);
    smartcap_fsm_tick(&f, 3000); // fast hunt completes -> RESCORE -> FOCUS -> ATTACK A
    CHECK(smartcap_fsm_stage(&f) == SMCAP_STAGE_LISTEN, "attacking the strongest target");

    // Complete A's handshake.
    pushEapol(&r, apA, 1, 4000); pushEapol(&r, apA, 2, 4000);
    pushEapol(&r, apA, 3, 4000); pushEapol(&r, apA, 4, 4000);
    smartcap_fsm_tick(&f, 4000);
    CHECK(smartcap_fsm_stage(&f) == SMCAP_STAGE_LISTEN, "now listening on target B");
    pushEapol(&r, apB, 1, 4500); pushEapol(&r, apB, 2, 4500);
    pushEapol(&r, apB, 3, 4500); pushEapol(&r, apB, 4, 4500);
    smartcap_fsm_tick(&f, 4500);
    CHECK(smartcap_fsm_stage(&f) == SMCAP_STAGE_LISTEN, "now listening on target C");
    pushEapol(&r, apC, 1, 5000); pushEapol(&r, apC, 2, 5000);
    pushEapol(&r, apC, 3, 5000); pushEapol(&r, apC, 4, 5000);
    smartcap_fsm_tick(&f, 5000);

    CHECK(gNCaptured == 3, "all three targets captured");
    CHECK(gNHs == 3 && gNPm == 0, "all captures were handshakes");
    CHECK(memcmp(gCaptured[0], apA, 6) == 0, "capture order A first");
    CHECK(memcmp(gCaptured[1], apB, 6) == 0, "capture order B second");
    CHECK(memcmp(gCaptured[2], apC, 6) == 0, "capture order C last");

    // Every attack went through the adapter as dry-run (counters stayed 0).
    uint32_t ctr[4];
    smartcap_radio_counters(&r, ctr);
    CHECK(ctr[1] == 0, "no handshake-triggering deauth was ever really sent");
    CHECK(occurrences("dry-run: deauth_client") == 3, "three would-be targeted deauths logged");

    smartcap_target_t *tA = smartcap_table_find(&t, apA);
    CHECK(tA && (tA->flags & SMCAP_HAVE_HS), "target A flagged as captured");
}

static void testFsTimeoutCooldown(void) {
    printf("fsm: LISTEN timeout -> COOLDOWN -> penalty per target:\n");
    smartcap_radio_t r;
    smartcap_radio_init(&r, &gOps);
    smartcap_radio_set_logger(&r, captureLog);
    smartcap_table_t t;
    smartcap_table_init(&t);
    smartcap_score_params_t sp;
    smartcap_score_params_default(&sp);
    smartcap_fsm_params_t fp;
    smartcap_fsm_params_default(&fp);
    smartcap_fsm_t f = makeEmptyFsm(&r, &t, &sp, &fp);

    uint8_t apX[6], clX[6];
    setMac(apX, 0x0a, 0x0a, 0x0a);
    setMac(clX, 0x0b, 0x0e, 0x0e);
    pushAp(&r, apX, 6, -50, "X", 0);
    pushClient(&r, apX, 6, -50, clX, 0);

    smartcap_fsm_tick(&f, 0);
    smartcap_fsm_tick(&f, 1000);
    smartcap_fsm_tick(&f, 2000);
    smartcap_fsm_tick(&f, 3000); // -> ATTACK X -> LISTEN (until 7000)

    smartcap_fsm_tick(&f, 4000); // mid-listen, no result yet
    CHECK(smartcap_fsm_stage(&f) == SMCAP_STAGE_LISTEN, "still waiting for the handshake");

    smartcap_target_t *tx = smartcap_table_find(&t, apX);
    CHECK(tx->attack_count == 0, "no failure recorded before the timeout");

    smartcap_fsm_tick(&f, 7000); // listen deadline reached
    CHECK(smartcap_fsm_stage(&f) == SMCAP_STAGE_COOLDOWN, "timeout -> cooldown");
    CHECK(tx->last_attack_ms == 7000 && tx->attack_count == 1,
          "failure marked on the target used by scoring");

    smartcap_fsm_tick(&f, 9000); // cooldown over, no other targets
    // A clientful target survives the penalty (still score>0 without rivals) and
    // gets retried - paced, not wedged. That is the intent: high-value targets
    // come back, plus cooldown ensures attempts are spaced out.
    CHECK(smartcap_fsm_stage(&f) == SMCAP_STAGE_LISTEN, "clientful target is retried after cooldown");
    CHECK(tx->attack_count == 1, "second attempt not yet marked a failure");

    smartcap_fsm_tick(&f, 13000); // second listen timeout
    CHECK(smartcap_fsm_stage(&f) == SMCAP_STAGE_COOLDOWN, "second timeout -> cooldown again");
    CHECK(tx->attack_count == 2, "second failure recorded");

    // Endless happiness: huge irregular jumps must not wedge the machine.
    int stage = -1;
    for (uint32_t now = 9000; now < 400000; now += 9997) {
        smartcap_fsm_tick(&f, now);
        stage = smartcap_fsm_stage(&f);
        CHECK(stage >= SMCAP_STAGE_SCAN && stage <= SMCAP_STAGE_COOLDOWN, "stage stays valid");
    }
    (void)stage;
}

static smartcap_stage_t gCbStage = (smartcap_stage_t)0xFF;
static uint32_t gCbParam = 0;

static void captureStageCb(const smartcap_fsm_t *f, smartcap_stage_t stage, uint32_t param_ms,
                           void *ctx) {
    (void)f;
    (void)ctx;
    gCbStage = stage;
    gCbParam = param_ms;
}

static void testFsStageCb(void) {
    printf("fsm: stage-entry callback observes COOLDOWN with its duration:\n");
    smartcap_radio_t r;
    smartcap_radio_init(&r, &gOps);
    smartcap_radio_set_logger(&r, captureLog);
    smartcap_table_t t;
    smartcap_table_init(&t);
    smartcap_score_params_t sp;
    smartcap_score_params_default(&sp);
    smartcap_fsm_params_t fp;
    smartcap_fsm_params_default(&fp);
    smartcap_fsm_t f = makeEmptyFsm(&r, &t, &sp, &fp);
    smartcap_fsm_set_cb(&f, captureStageCb, (void *)0xABCD);

    uint8_t ap[6], cli[6];
    setMac(ap, 0x0e, 0x0e, 0x0e);
    setMac(cli, 0x0f, 0x0f, 0x0f);
    pushAp(&r, ap, 6, -50, "C", 0);
    pushClient(&r, ap, 6, -50, cli, 0);

    smartcap_fsm_tick(&f, 0);
    smartcap_fsm_tick(&f, 1000);
    smartcap_fsm_tick(&f, 2000);
    smartcap_fsm_tick(&f, 3000); // -> ATTACK -> LISTEN until 7000

    gCbStage = (smartcap_stage_t)0xFF;
    smartcap_fsm_tick(&f, 7000); // listen timeout -> COOLDOWN
    CHECK(gCbStage == SMCAP_STAGE_COOLDOWN, "stage cb fires on entering COOLDOWN");
    CHECK(gCbParam == fp.cooldown_ms, "cb carries the cooldown duration ms");

    gCbStage = (smartcap_stage_t)0xFF;
    smartcap_fsm_tick(&f, 9000); // cooldown over -> retry
    CHECK(gCbStage != SMCAP_STAGE_COOLDOWN, "retry after cooldown is not another cooldown");
}

static void testFsPmkidPassive(void) {
    printf("fsm: passive PMKID strategy + PMKID capture:\n");
    smartcap_radio_t r;
    smartcap_radio_init(&r, &gOps);
    smartcap_radio_set_logger(&r, captureLog);
    smartcap_table_t t;
    smartcap_table_init(&t);
    smartcap_score_params_t sp;
    smartcap_score_params_default(&sp);
    smartcap_fsm_params_t fp;
    smartcap_fsm_params_default(&fp);
    smartcap_fsm_t f = makeEmptyFsm(&r, &t, &sp, &fp);

    uint8_t apY[6];
    setMac(apY, 0x0c, 0x0c, 0x0c);
    pushAp(&r, apY, 11, -50, "Y", 0);

    smartcap_result_set_handshake_fn(hsHook);
    smartcap_result_set_pmkid_fn(pmHook);
    resetCapture();
    resetLog();

    smartcap_fsm_tick(&f, 0);
    smartcap_fsm_tick(&f, 1000);
    // Mark the AP as cooperating with the silent method (observed via probe).
    smartcap_target_t *ty = smartcap_table_find(&t, apY);
    ty->flags |= SMCAP_PMKID_METHOD;
    ty->last_seen_ms = 1000;

    smartcap_fsm_tick(&f, 2000);
    smartcap_fsm_tick(&f, 3000); // RESCORE -> FOCUS -> ATTACK (assoc-pmkid) -> LISTEN
    CHECK(smartcap_fsm_stage(&f) == SMCAP_STAGE_LISTEN, "PMKID strategy listening");
    CHECK(occurrences("assoc_pmkid") == 1, "assoc/PMKID would-be sent and logged");

    pushPmkid(&r, apY, 4000);
    smartcap_fsm_tick(&f, 4000); // drain -> capture
    CHECK(gNPm == 1 && gNCaptured == 1, "PMKID captured and reported");
    smartcap_target_t *t2 = smartcap_table_find(&t, apY);
    CHECK(t2 && (t2->flags & SMCAP_HAVE_PMKID), "target flagged with HAVE_PMKID");

    uint32_t ctr[4];
    smartcap_radio_counters(&r, ctr);
    CHECK(ctr[3] == 0, "assoc request never really sent in dry-run");
}

static void testFsPmkidFlagFromBeacon(void) {
    printf("fsm: RSN beacon flags target; live assoc carries ap+ssid:\n");
    smartcap_radio_t r;
    smartcap_radio_init(&r, &gOps);
    smartcap_radio_set_logger(&r, captureLog);
    smartcap_table_t t;
    smartcap_table_init(&t);
    smartcap_score_params_t sp;
    smartcap_score_params_default(&sp);
    smartcap_fsm_params_t fp;
    smartcap_fsm_params_default(&fp);
    smartcap_fsm_t f = makeEmptyFsm(&r, &t, &sp, &fp);
    resetLog();

    uint8_t ap[6];
    setMac(ap, 0x31, 0x31, 0x31);

    // Feed the beacon through the real parser, not a synthetic event.
    uint8_t frame[128];
    size_t len = 0;
    buildBeaconRsn(frame, ap, "WPA", true, &len);
    CHECK(smartcap_radio_report_frame(&r, frame, (uint16_t)len, -50, 11, 0),
          "RSN beacon accepted");

    smartcap_fsm_tick(&f, 0);     // drain AP_SEEN, score the target
    smartcap_target_t *ta = smartcap_table_find(&t, ap);
    CHECK(ta, "beacon created the target");
    CHECK(ta->flags & SMCAP_PMKID_METHOD, "target flagged PMKID-capable from its beacon");

    // Live mode: once the strategy fires, the assoc request must reach the
    // mock backend carrying the AP + the SSID it used in its RSN claim. (The
    // fast hunt returns to RESCORE on the third channel hop, which is the
    // natural path into FOCUS/ATTACK here.)
    smartcap_radio_set_dry_run(&r, false);
    gNAssoc = 0;
    gLastAssocSsid[0] = '\0';
    smartcap_fsm_tick(&f, 1000);
    smartcap_fsm_tick(&f, 2000);
    smartcap_fsm_tick(&f, 3000); // RESCORE -> FOCUS -> ATTACK -> LISTEN
    CHECK(gNAssoc == 1, "assoc request injected exactly once in live mode");
    CHECK(memcmp(gLastAp, ap, 6) == 0, "assoc targeted the flagged AP");
    CHECK(strcmp(gLastAssocSsid, "WPA") == 0, "assoc carries the beacon SSID");
}

// The regression: one station seen talking to AP A and then AP B (multi-SSID
// router, same client) was being tracked under BOTH entries; the FSM then
// attacked it alternately on both BSSIDs. A station belongs to exactly one AP,
// and the freshest sighting wins.
static void testClientMigration(void) {
    printf("table/fsm: one client belongs to exactly one AP, freshest sighting wins:\n");
    smartcap_radio_t r;
    smartcap_radio_init(&r, &gOps);
    smartcap_table_t t;
    smartcap_table_init(&t);
    smartcap_score_params_t sp;
    smartcap_score_params_default(&sp);
    smartcap_fsm_params_t fp;
    smartcap_fsm_params_default(&fp);
    smartcap_fsm_t f = makeEmptyFsm(&r, &t, &sp, &fp);

    uint8_t bssidA[6], bssidB[6], client[6];
    setMac(bssidA, 0x41, 0x41, 0x41);
    setMac(bssidB, 0x42, 0x42, 0x42);
    setMac(client, 0x44, 0x43, 0x43);

    // Frame 1: client -> BSSID A (toDS). Frame 2 (later): client -> BSSID B.
    uint8_t frame[64];
    size_t len = 0;
    buildDataToDs(frame, bssidA, client, &len);
    CHECK(smartcap_radio_report_frame(&r, frame, (uint16_t)len, -50, 6, 1000), "frame A accepted");
    buildDataToDs(frame, bssidB, client, &len);
    CHECK(smartcap_radio_report_frame(&r, frame, (uint16_t)len, -45, 6, 2000), "frame B accepted");

    smartcap_fsm_tick(&f, 1000);
    smartcap_fsm_tick(&f, 2000);
    smartcap_target_t *ta = smartcap_table_find(&t, bssidA);
    smartcap_target_t *tb = smartcap_table_find(&t, bssidB);
    CHECK(ta && ta->n_clients == 0, "after the B sighting, the client is gone from AP A");
    CHECK(tb && tb->n_clients == 1, "client is tracked under AP B only");
    if (tb && tb->n_clients == 1) {
        CHECK(memcmp(tb->clients[0].mac, client, 6) == 0, "AP B holds the station MAC");
    }
}

static void testAttackRivalsDiag(void) {
    printf("fsm: ATTACK diagnostics list the outside-focus contenders:\n");
    smartcap_radio_t r;
    smartcap_radio_init(&r, &gOps);
    smartcap_radio_set_logger(&r, captureLog);
    smartcap_table_t t;
    smartcap_table_init(&t);
    smartcap_score_params_t sp;
    smartcap_score_params_default(&sp);
    smartcap_fsm_params_t fp;
    smartcap_fsm_params_default(&fp);

    // 6 open targets on channel 6, descending RSSI. SMCAP_MAX_FOCUS is 4, so
    // ranks 5 and 6 fall outside the focus and must show up in the log.
    uint8_t ap[6][6], cli[6][6];
    for (int i = 0; i < 6; ++i) {
        setMac(ap[i], (uint8_t)(0xa0 + i), 0x01, 0x01);
        setMac(cli[i], (uint8_t)(0xb0 + i), 0x02, 0x02);
        pushAp(&r, ap[i], 6, (int8_t)(-45 - 5 * i), "R", 0);
        pushClient(&r, ap[i], 6, (int8_t)(-45 - 5 * i), cli[i], 0);
    }

    smartcap_fsm_t f = makeEmptyFsm(&r, &t, &sp, &fp);
    resetLog();
    smartcap_fsm_tick(&f, 0);
    smartcap_fsm_tick(&f, 1000);
    smartcap_fsm_tick(&f, 2000);
    smartcap_fsm_tick(&f, 3000); // fast hunt done -> focus -> ATTACK best target
    CHECK(smartcap_fsm_stage(&f) == SMCAP_STAGE_LISTEN, "attacking the top-scored target");
    CHECK(occurrences("score=") == 1, "ATTACK line carries the target's score");
    CHECK(occurrences("rivals-outside-focus") == 1, "rivals line is emitted");

    // Ranks 5 (rssi -65) and 6 (rssi -70) are outside the 4-entry focus.
    char needle[32];
    snprintf(needle, sizeof(needle), "%02X:%02X:...:%02X=", 0xa4, 0x01, 0x88);
    CHECK(occurrences(needle) == 1, "first outside-focus contender is listed");
    snprintf(needle, sizeof(needle), "%02X:%02X:...:%02X=", 0xa5, 0x01, 0x88);
    CHECK(occurrences(needle) == 1, "second outside-focus contender is listed");
}

static void testFsWarnStreak(void) {
    printf("fsm: WARN after 3 consecutive failed attempts on one target:\n");
    smartcap_radio_t r;
    smartcap_radio_init(&r, &gOps);
    smartcap_radio_set_logger(&r, captureLog);
    smartcap_table_t t;
    smartcap_table_init(&t);
    smartcap_score_params_t sp;
    smartcap_score_params_default(&sp);
    smartcap_fsm_params_t fp;
    smartcap_fsm_params_default(&fp);

    uint8_t ap[6];
    setMac(ap, 0x51, 0x51, 0x51);
    pushAp(&r, ap, 6, -50, "Z", 0);
    // 5 clients: strong enough that the streak penalty does not suppress the
    // target below zero before the third failure (so the WARN can fire).
    for (int i = 0; i < 5; ++i) {
        uint8_t c[6];
        setMac(c, (uint8_t)(0x52 + i), 0x52, 0x52);
        pushClient(&r, ap, 6, -50, c, 0);
    }

    smartcap_fsm_t f = makeEmptyFsm(&r, &t, &sp, &fp);
    resetLog();
    smartcap_fsm_tick(&f, 0);
    smartcap_fsm_tick(&f, 1000);
    smartcap_fsm_tick(&f, 2000);
    smartcap_fsm_tick(&f, 3000); // -> ATTACK -> LISTEN (until 7000)
    CHECK(occurrences("timeout after") == 0, "no failure yet");

    smartcap_fsm_tick(&f, 7000);  // failure 1
    CHECK(occurrences("WARN:") == 0, "no warn after one failure");
    smartcap_fsm_tick(&f, 9000);  // retry
    smartcap_fsm_tick(&f, 13000); // failure 2
    CHECK(occurrences("WARN:") == 0, "no warn after two failures");
    smartcap_fsm_tick(&f, 15000); // retry
    smartcap_fsm_tick(&f, 19000); // failure 3 -> WARN
    CHECK(occurrences("WARN: target") == 1, "WARN emitted once after the third consecutive failure");
    smartcap_target_t *tz = smartcap_table_find(&t, ap);
    CHECK(tz && tz->attack_count == 3, "three failures recorded on the target");
}

// Hard exclusion: a target that keeps failing is withheld from the top-N
// selection even when its raw score is the highest in the table, then returns
// once the backoff window serves out.
static void testExclusionFromRotation(void) {
    printf("focus: hard exclusion overrides raw score, then expires:\n");
    smartcap_table_t t;
    smartcap_table_init(&t);
    smartcap_score_params_t sp;
    smartcap_score_params_default(&sp);

    uint8_t apA[6], apB[6];
    setMac(apA, 0x61, 0x61, 0x61);
    setMac(apB, 0x62, 0x62, 0x62);

    // A: 5 clients + strong RSSI -> dominates the raw score, but has failed a lot.
    smartcap_target_t *ta = smartcap_table_upsert(&t, apA);
    ta->n_clients = 5;
    ta->rssi = -45;
    ta->last_seen_ms = 1000;
    ta->consecutive_failures = 6; // >= exclude_after_failures
    ta->last_attack_ms = 500;     // recent: recovery does not kick in
    // B: weaker (1 client, worse signal) but healthy.
    smartcap_target_t *tb = smartcap_table_upsert(&t, apB);
    tb->n_clients = 1;
    tb->rssi = -70;
    tb->last_seen_ms = 1000;

    // Sanity: without the streak, A's raw score would beat B.
    smartcap_target_t copy = *ta;
    copy.consecutive_failures = 0;
    copy.last_attack_ms = 0;
    CHECK(smartcap_score(&sp, &copy, 2000) > smartcap_score(&sp, tb, 2000),
          "A's raw score exceeds B's");

    smartcap_focus_t f;
    resetLog();
    const uint8_t n = smartcap_focus_build(&t, &sp, 2000, &f, focusLogHook, NULL);
    CHECK(n == 1 && f.count == 1, "only the healthy target is selected");
    CHECK(memcmp(f.entries[0].bssid, apB, 6) == 0,
          "A is absent from focus despite the higher raw score");
    CHECK(occurrences("excluded from rotation") == 1, "exclusion is logged");
    CHECK(ta->exclusion_count == 1 && ta->excluded_until_ms > 2000,
          "backoff window opened with period count 1");

    // Once the window serves out, A's streak clears and its raw score brings it
    // back on top.
    resetLog();
    const uint32_t after = ta->excluded_until_ms + 1;
    smartcap_focus_t f2;
    const uint8_t n2 = smartcap_focus_build(&t, &sp, after, &f2, focusLogHook, NULL);
    CHECK(n2 >= 1, "something is selected again after the window");
    CHECK(memcmp(f2.entries[0].bssid, apA, 6) == 0, "A is back on top once reconsidered");
    CHECK(ta->consecutive_failures == 0, "streak cleared on re-inclusion");
    CHECK(ta->exclusion_count == 1, "period count kept so the next backoff is longer");
    CHECK(occurrences("excluded from rotation") == 0, "no new exclusion while fresh");
}

static void testExclusionExpiryReturns(void) {
    printf("focus: recovery (no attempts for fail_reset_ms) fully clears a target:\n");
    smartcap_table_t t;
    smartcap_table_init(&t);
    smartcap_score_params_t sp;
    smartcap_score_params_default(&sp);

    uint8_t apA[6];
    setMac(apA, 0x63, 0x63, 0x63);
    smartcap_target_t *ta = smartcap_table_upsert(&t, apA);
    ta->n_clients = 3;
    ta->rssi = -50;
    ta->last_seen_ms = 1;
    ta->consecutive_failures = 8;
    ta->exclusion_count = 2;
    ta->excluded_until_ms = 100000;
    ta->last_attack_ms = 1;

    // At t=1000 the streak is long and the exclusion is still active.
    smartcap_focus_t f1;
    const uint8_t n1 = smartcap_focus_build(&t, &sp, 1000, &f1, NULL, NULL);
    CHECK(n1 == 0, "still excluded while under the backoff window");

    // After fail_reset_ms of silence the target fully recovers regardless of
    // how many periods it had racked up.
    const uint32_t recovered = ta->last_attack_ms + sp.fail_reset_ms + 1;
    smartcap_focus_t f2;
    const uint8_t n2 = smartcap_focus_build(&t, &sp, recovered, &f2, NULL, NULL);
    CHECK(n2 == 1 && memcmp(f2.entries[0].bssid, apA, 6) == 0,
          "recovered target is back in rotation");
    CHECK(ta->consecutive_failures == 0 && ta->exclusion_count == 0 &&
          ta->excluded_until_ms == 0,
          "recovery cleared streak and period counters");
}

static void testExclusionPassiveCapture(void) {
    printf("fsm: passive handshake still closes a target under exclusion:\n");
    smartcap_radio_t r;
    smartcap_radio_init(&r, &gOps);
    smartcap_radio_set_logger(&r, captureLog);
    smartcap_table_t t;
    smartcap_table_init(&t);
    smartcap_score_params_t sp;
    smartcap_score_params_default(&sp);
    smartcap_fsm_params_t fp;
    smartcap_fsm_params_default(&fp);
    smartcap_fsm_t f = makeEmptyFsm(&r, &t, &sp, &fp);

    uint8_t apA[6], cli[6];
    setMac(apA, 0x71, 0x71, 0x71);
    setMac(cli, 0x72, 0x72, 0x72);
    pushAp(&r, apA, 6, -50, "A", 0);
    pushClient(&r, apA, 6, -50, cli, 0);

    smartcap_result_set_handshake_fn(hsHook);
    smartcap_result_set_pmkid_fn(pmHook);
    resetCapture();
    resetLog();

    smartcap_fsm_tick(&f, 0);
    smartcap_fsm_tick(&f, 1000);
    smartcap_fsm_tick(&f, 2000);

    // Simulate the failing streak and drop the target into exclusion.
    smartcap_target_t *ta = smartcap_table_find(&t, apA);
    CHECK(ta != NULL, "target tracked");
    ta->consecutive_failures = 6;
    ta->last_attack_ms = 1500;
    ta->excluded_until_ms = 0;

    // The next focus pass must withhold the (only) target -> empty focus.
    smartcap_fsm_tick(&f, 3000);
    CHECK(occurrences("excluded from rotation") == 1, "FSM logged the exclusion");
    CHECK(smartcap_fsm_stage(&f) != SMCAP_STAGE_LISTEN &&
          smartcap_fsm_stage(&f) != SMCAP_STAGE_ATTACK &&
          smartcap_fsm_stage(&f) != SMCAP_STAGE_COOLDOWN,
          "excluded target is not attacked (empty focus -> scan)");

    // A passive 4-way handshake must still be captured and close the target.
    pushEapol(&r, apA, 1, 4000);
    pushEapol(&r, apA, 2, 4000);
    pushEapol(&r, apA, 3, 4000);
    pushEapol(&r, apA, 4, 4000);
    smartcap_fsm_tick(&f, 4000);
    CHECK(gNHs == 1 && gNCaptured == 1, "passive handshake captured");
    CHECK(ta->flags & SMCAP_HAVE_HS, "excluded target closed by the passive capture");
    CHECK(ta->consecutive_failures == 0, "capture cleared the failure streak");
}

int main(void) {
    printf("SmartCap radio + FSM host test\n");
    testAdapterGating();
    testAdapterParsing();
    testAdapterPmkidRsn();
    testRadioNotifyGate();
    testFsEmptyFocusHunts();
    testFsExternalRescore();
    testFsMultiTargetGroup();
    testFsTimeoutCooldown();
    testFsStageCb();
    testFsPmkidPassive();
    testFsPmkidFlagFromBeacon();
    testClientMigration();
    testAttackRivalsDiag();
    testFsWarnStreak();
    testExclusionFromRotation();
    testExclusionExpiryReturns();
    testExclusionPassiveCapture();
    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}