// SmartCap radio adapter core. Pure translation, no decisions, host-compilable.
// The only seams to the outside are:
//   - radio_ops_t (provided by the firmware for live mode, or a mock in tests)
//   - smartcap_log_fn (optional; dry-run actions go through it)

#include "smartcap_radio.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void smartcap_radio_init(smartcap_radio_t *r, const smartcap_radio_ops_t *ops) {
    memset(r, 0, sizeof(*r));
    r->ops = ops;
    r->dry_run = SMCAP_RADIO_DEFAULT_DRY_RUN;
    r->allow_broadcast = false;
    r->virt_channel = 1;
    r->selected_channel = 1;
}

void smartcap_radio_set_logger(smartcap_radio_t *r, smartcap_log_fn fn) { r->logger = fn; }
void smartcap_radio_set_notify(smartcap_radio_t *r, smartcap_radio_notify_fn fn, void *ctx) {
    r->notify = fn;
    r->notify_ctx = ctx;
}
void smartcap_radio_set_event_gate(smartcap_radio_t *r, smartcap_radio_event_gate_fn fn, void *ctx) {
    r->event_gate = fn;
    r->event_gate_ctx = ctx;
}
void smartcap_radio_set_dry_run(smartcap_radio_t *r, bool on) { r->dry_run = on; }
void smartcap_radio_set_allow_broadcast(smartcap_radio_t *r, bool on) { r->allow_broadcast = on; }
bool smartcap_radio_is_dry_run(const smartcap_radio_t *r) { return r->dry_run; }

void smartcap_radio_counters(const smartcap_radio_t *r, uint32_t out[4]) {
    out[0] = r->n_set_channel;
    out[1] = r->n_deauth_client;
    out[2] = r->n_deauth_bcast;
    out[3] = r->n_assoc_pmkid;
}

// ---------------------------------------------------------------------------
// SPSC event queue (one producer = the wifi task/parser, one consumer = FSM).
// ---------------------------------------------------------------------------
static size_t ringNext(size_t i) { return (i + 1) % SMCAP_RADIO_QUEUE; }

static bool ringPush(smartcap_radio_t *r, const smartcap_radio_event_t *ev) {
    if (r->event_gate && !r->event_gate(ev, r->event_gate_ctx)) {
        return false; // firmware gate (e.g. whitelist) rejected the event
    }
    if (ringNext(r->prod) == r->cons) {
        return false; // full -> drop (frame loss is fine in this design)
    }
    r->queue[r->prod] = *ev;
    r->prod = ringNext(r->prod);
    return true;
}

static bool ringPop(smartcap_radio_t *r, smartcap_radio_event_t *out) {
    if (r->cons == r->prod) {
        return false;
    }
    *out = r->queue[r->cons];
    r->cons = ringNext(r->cons);
    return true;
}

bool smartcap_radio_push_event(smartcap_radio_t *r, const smartcap_radio_event_t *ev) {
    return ringPush(r, ev);
}

bool smartcap_radio_poll(smartcap_radio_t *r, smartcap_radio_event_t *out) {
    return ringPop(r, out);
}

// ---------------------------------------------------------------------------
// Dry-run TX: log with full context, never touch the backend.
// ---------------------------------------------------------------------------
static void logLine(smartcap_radio_t *r, const char *fmt, ...) {
    if (!r->logger) {
        return;
    }
    char line[SMCAP_RADIO_LOGLINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    r->logger(line);
}

// ---------------------------------------------------------------------------
// RX parsing. Mirrors the firmware's own sniffing helpers but keeps the
// adapter self-contained and host-testable.
// ---------------------------------------------------------------------------

// For data frames: does the LLC/SNAP header carry an EAPOL ethertype (0x888e)?
// On success also outputs the MAC header length so the caller can reach the
// EAPOL descriptor. Logic matches isItEapol() in the firmware.
static bool frameIsEapol(const uint8_t *frame, int len, int *hdrLen) {
    if (len < 36) {
        return false;
    }
    const uint16_t fc = (uint16_t)frame[0] | ((uint16_t)frame[1] << 8);
    const uint8_t type = (fc & 0x0C) >> 2;
    if (type != 0x02) {
        return false;
    }

    int hdr = 24;
    const bool toDs = frame[1] & 0x01;
    const bool fromDs = frame[1] & 0x02;
    if (toDs && fromDs) { hdr += 6; }
    const uint8_t subtype = (fc & 0xF0) >> 4;
    if (subtype & 0x08) { hdr += 2; }
    if (frame[1] & 0x80) { hdr += 4; }

    if (len < hdr + 8) {
        return false;
    }
    const uint8_t *llc = frame + hdr;
    const int llcLen = len - hdr;
    if (llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03 && llc[3] == 0x00 &&
        llc[4] == 0x00 && llc[5] == 0x00) {
        const uint8_t *eth = llc + 6;
        if (eth[0] == 0x88 && eth[1] == 0x8E) { *hdrLen = hdr; return true; }
        if (eth[0] == 0x81 && eth[1] == 0x00 && llcLen >= 12) {
            const uint8_t *inner = eth + 4;
            if (inner[0] == 0x88 && inner[1] == 0x8E) { *hdrLen = hdr; return true; }
        }
    }
    for (int i = hdr; i <= len - 2; ++i) {
        if (frame[i] == 0x88 && frame[i + 1] == 0x8E) { *hdrLen = hdr; return true; }
    }
    return false;
}

// 4-way handshake classifier. Returns 1..4 or -1 (mirrors classifyEapolMessage).
static int frameEapolMsg(const uint8_t *payload, int sigLen) {
    const int qosOffset = ((payload[0] & 0x0F) == 0x08) ? 2 : 0;
    const int keyInfoOffset = 24 + qosOffset + 8 + 4 + 1;
    if (sigLen < keyInfoOffset + 2) {
        return -1;
    }
    const uint16_t keyInfo = (uint16_t)(payload[keyInfoOffset] << 8) | payload[keyInfoOffset + 1];
    const bool install = keyInfo & (1 << 6);
    const bool ack = keyInfo & (1 << 7);
    const bool mic = keyInfo & (1 << 8);
    const bool secure = keyInfo & (1 << 9);
    if (ack && !mic && !install) return 1;
    if (!ack && mic && !install && !secure) return 2;
    if (ack && mic && install) return 3;
    if (!ack && mic && !install && secure) return 4;
    return -1;
}

static bool isBroadcast(const uint8_t *mac) {
    return mac[0] == 0xFF && mac[1] == 0xFF && mac[2] == 0xFF &&
           mac[3] == 0xFF && mac[4] == 0xFF && mac[5] == 0xFF;
}

// Walk the information elements of a management frame body (ssid etc.). The
// frame layout is: header(24) + fixed fields + IEs. Returns a pointer to the
// IE value, or nullptr; also fills *outIeLen with the length byte.
static const uint8_t *findIe(const uint8_t *frame, size_t pos, size_t frameLen,
                             uint8_t tag, uint8_t *outIeLen) {
    while (pos + 2 <= frameLen) {
        const uint8_t t = frame[pos];
        const uint8_t l = frame[pos + 1];
        if (pos + 2 + l > frameLen) {
            return nullptr;
        }
        if (t == tag) {
            *outIeLen = l;
            return frame + pos + 2;
        }
        pos += 2 + l;
    }
    return nullptr;
}

// Minimal RSN IE parse (WPA2/WPA3): flags whether a PSK AKM suite is present
// and whether the IE carries a PMKID list. A PMKID list only ever appears in
// associaion responses, never in beacons.
typedef struct {
    bool psk_akm;
    uint16_t pmkid_count;
} rsn_info_t;

static void parseRsn(const uint8_t *ie, uint8_t ieLen, rsn_info_t *out) {
    memset(out, 0, sizeof(*out));
    size_t pos = 0;
    if (ieLen < 8) { // version(2) + group(4) + pairwise count(2)
        return;
    }
    pos = 6;
    const uint16_t pcnt = (uint16_t)(ie[pos] | ((uint16_t)ie[pos + 1] << 8));
    pos += 2;
    if (pos + (size_t)pcnt * 4 + 2 > ieLen) { return; }
    pos += (size_t)pcnt * 4;
    const uint16_t acnt = (uint16_t)(ie[pos] | ((uint16_t)ie[pos + 1] << 8));
    pos += 2;
    if (pos + (size_t)acnt * 4 + 2 > ieLen) { return; }
    for (uint16_t i = 0; i < acnt; ++i) {
        const uint8_t *akm = ie + pos + (size_t)i * 4;
        if (akm[0] == 0x00 && akm[1] == 0x0f && akm[2] == 0xac && akm[3] == 0x02) {
            out->psk_akm = true;
        }
    }
    pos += (size_t)acnt * 4;
    if (pos + 2 > ieLen) { return; }
    pos += 2; // RSN capabilities
    if (pos + 2 > ieLen) { return; }
    out->pmkid_count = (uint16_t)(ie[pos] | ((uint16_t)ie[pos + 1] << 8));
}

bool smartcap_radio_report_frame(smartcap_radio_t *r, const uint8_t *frame,
                                 uint16_t len, int8_t rssi, uint8_t channel,
                                 uint32_t now_ms) {
    if (!frame || len < 10) {
        return false;
    }
    const bool fcsIncluded = len >= 4;
    const int pktLen = static_cast<int>(fcsIncluded ? len - 4 : len);
    if (pktLen < 24) {
        return false;
    }

    const uint16_t fc = (uint16_t)frame[0] | ((uint16_t)frame[1] << 8);
    const uint8_t type = (fc & 0x0C) >> 2;
    const uint8_t subtype = (fc & 0xF0) >> 4;

    smartcap_radio_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.ts_ms = now_ms;
    ev.rssi = rssi;
    ev.channel = channel;

    // --- management: beacons (subtype 8) -> AP_SEEN -----------------------
    if (type == 0x00 && subtype == 0x08) {
        ev.type = SMCAP_EV_AP_SEEN;
        memcpy(ev.bssid, frame + 10, 6); // sa (bssid for beacons)
        memcpy(ev.mac, ev.bssid, 6);
        if (pktLen >= 36) {
            size_t pos = 36;
            while (pos + 2 <= (size_t)pktLen) {
                const uint8_t tag = frame[pos];
                const uint8_t tagLen = frame[pos + 1];
                if (pos + 2 + tagLen > (size_t)pktLen) { break; }
                if (tag == 0) { // SSID
                    const size_t sl = tagLen < SMCAP_SSID_MAX - 1 ? tagLen : SMCAP_SSID_MAX - 1;
                    memcpy(ev.ssid, frame + pos + 2, sl);
                    ev.ssid[sl] = '\0';
                }
                pos += 2 + tagLen;
            }
            // RSN IE (tag 48): a WPA2/WPA3 network, so the silent assoc-request
            // trick may be answered with a PMKID KDE.
            uint8_t ieLen = 0;
            const uint8_t *rsn = findIe(frame, 36, (size_t)pktLen, 48, &ieLen);
            rsn_info_t info;
            (void)info;
            if (rsn) {
                parseRsn(rsn, ieLen, &info);
                ev.pmkid_method = true;
            }
        }
        return ringPush(r, &ev);
    }

    // --- management: association response (subtype 1) ----------------------
    // The reply to our injected assoc request. When the AP honored the PMKID
    // KDE request its RSN IE carries the PMKID list; that is a capture.
    if (type == 0x00 && subtype == 0x01) {
        uint8_t ieLen = 0;
        const uint8_t *rsn = findIe(frame, 30, (size_t)pktLen, 48, &ieLen);
        if (rsn) {
            rsn_info_t info;
            parseRsn(rsn, ieLen, &info);
            if (info.pmkid_count > 0) {
                ev.type = SMCAP_EV_PMKID_SEEN;
                memcpy(ev.bssid, frame + 10, 6); // SA = the AP
                memcpy(ev.mac, ev.bssid, 6);
                return ringPush(r, &ev);
            }
        }
        return false;
    }

    // --- data frames --------------------------------------------------------
    if (type == 0x02) {
        const bool toDs = frame[1] & 0x01;
        const bool fromDs = frame[1] & 0x02;

        // EAPOL handshake progress.
        int hdrLen = 0;
        if (frameIsEapol(frame, pktLen, &hdrLen)) {
            // AP = the address that equals the BSSID field (addr3), same pick
            // the pcap path uses: dest==bssid ? dest : src.
            const uint8_t *dest = frame + 4;
            const uint8_t *src = frame + 10;
            const uint8_t *apField = frame + 16;
            const uint8_t *ap = (memcmp(dest, apField, 6) == 0) ? dest : src;
            const int msg = frameEapolMsg(frame, pktLen);
            if (msg >= 1 && msg <= 4) {
                ev.type = SMCAP_EV_EAPOL_SEEN;
                ev.msg = (uint8_t)msg;
                memcpy(ev.bssid, ap, 6);
                memcpy(ev.mac, ap, 6);
                return ringPush(r, &ev);
            }
            return false;
        }

        // PMKID captures arrive as *management* association responses and are
        // handled in the mgmt branch above; non-EAPOL data frames only feed the
        // client-liveness path below.
        (void)subtype;
        (void)hdrLen;

        // Client liveness: for toDS frames the station is addr2, for fromDS it
        // is addr1. WDS/ad-hoc (both/neither DS bits) carry no clear client.
        if (toDs == fromDs) {
            return false;
        }
        const uint8_t *apField = frame + 16;
        const uint8_t *client = toDs ? frame + 10 : frame + 4;
        if (memcmp(client, apField, 6) == 0 || isBroadcast(client) ||
            (client[0] & 0x01) /* multicast */) {
            return false;
        }
        ev.type = SMCAP_EV_CLIENT_SEEN;
        memcpy(ev.bssid, apField, 6);
        memcpy(ev.mac, client, 6);
        return ringPush(r, &ev);
    }

    return false;
}

// ---------------------------------------------------------------------------
// TX: dry-run blows out as logging; counters only move in live mode.
// ---------------------------------------------------------------------------
bool smartcap_radio_get_channel(smartcap_radio_t *r, uint8_t *out) {
    if (!out) {
        return false;
    }
    if (r->dry_run || !r->ops) {
        *out = r->virt_channel; // FSM's virtual view, never a real radio read
        return true;
    }
    return r->ops->get_channel ? r->ops->get_channel(out) : false;
}

bool smartcap_radio_set_channel(smartcap_radio_t *r, uint8_t channel) {
    bool ok;
    if (r->dry_run || !r->ops || !r->ops->set_channel) {
        r->virt_channel = channel;
        logLine(r, "dry-run: set_channel(%u)", channel);
        ok = true; // the FSM's virtual state moved even though nothing was sent
    } else {
        ok = r->ops->set_channel(channel);
        if (ok) {
            ++r->n_set_channel;
        }
    }
    if (ok && r->selected_channel != channel) {
        r->selected_channel = channel;
        if (r->notify) {
            r->notify(SMCAP_RADIO_NOTIFY_CHANNEL, channel, nullptr, nullptr, r->notify_ctx);
        }
    }
    return ok;
}

void smartcap_radio_deauth_client(smartcap_radio_t *r, const uint8_t *ap, const uint8_t *client) {
    char apStr[18], cliStr[18];
    snprintf(apStr, sizeof(apStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             ap[0], ap[1], ap[2], ap[3], ap[4], ap[5]);
    snprintf(cliStr, sizeof(cliStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             client[0], client[1], client[2], client[3], client[4], client[5]);
    if (r->dry_run || !r->ops || !r->ops->deauth_client) {
        logLine(r, "dry-run: deauth_client(ch=%u, ap=%s, client=%s)",
                r->virt_channel, apStr, cliStr);
        return;
    }
    r->ops->deauth_client(ap, client);
    ++r->n_deauth_client;
    // Live action really went out; tell the firmware so UI/plugins can react.
    // Dry-run sends nothing, so it reports nothing either (observe-only means
    // no pretend attacks).
    if (r->notify) {
        r->notify(SMCAP_RADIO_NOTIFY_DEAUTH_SENT, 0, ap, apStr, r->notify_ctx);
    }
}

void smartcap_radio_deauth_bcast(smartcap_radio_t *r, const uint8_t *ap) {
    if (!r->allow_broadcast) {
        // Physically unreachable unless explicitly enabled - even in live mode.
        logLine(r, "BLOCKED: broadcast deauth is not allowed (ap=%02X:%02X:...:%02X)",
                ap[0], ap[1], ap[5]);
        return;
    }
    if (r->dry_run || !r->ops || !r->ops->deauth_bcast) {
        logLine(r, "dry-run: deauth_bcast(ch=%u, ap=%02X:%02X:%02X:%02X:%02X:%02X)",
                r->virt_channel, ap[0], ap[1], ap[2], ap[3], ap[4], ap[5]);
        return;
    }
    r->ops->deauth_bcast(ap);
    ++r->n_deauth_bcast;
    if (r->notify) {
        char apStr[18];
        snprintf(apStr, sizeof(apStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 ap[0], ap[1], ap[2], ap[3], ap[4], ap[5]);
        r->notify(SMCAP_RADIO_NOTIFY_DEAUTH_SENT, 0, ap, apStr, r->notify_ctx);
    }
}

void smartcap_radio_assoc_pmkid(smartcap_radio_t *r, const uint8_t *ap, const char *ssid) {
    if (r->dry_run || !r->ops || !r->ops->assoc_pmkid) {
        if (ssid && ssid[0]) {
            logLine(r, "dry-run: assoc_pmkid(ch=%u, ap=%02X:%02X:%02X:%02X:%02X:%02X, ssid=%.32s)",
                    r->virt_channel, ap[0], ap[1], ap[2], ap[3], ap[4], ap[5], ssid);
        } else {
            logLine(r, "dry-run: assoc_pmkid(ch=%u, ap=%02X:%02X:%02X:%02X:%02X:%02X)",
                    r->virt_channel, ap[0], ap[1], ap[2], ap[3], ap[4], ap[5]);
        }
        return;
    }
    r->ops->assoc_pmkid(ap, ssid);
    ++r->n_assoc_pmkid;
}