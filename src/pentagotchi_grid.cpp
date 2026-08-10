#include "pentagotchi_grid.h"

#include "pentagotchi_events.h"
#include "pentagotchi_internal.h"

#include <ArduinoJson.h>
#include <esp_log.h>
#include <esp_random.h>
#include <nvs.h>
#include <nvs_flash.h>

using namespace pentagotchi::detail;

static const char *TAG = "pentagotchi_grid";

static const char *kNvsNamespace = "pentagotchi";
static const char *kNvsIdentity = "identity";

// Pal-compatible beacon header (Bruce/PwnGridSpam): FC, dur, DA, SA=DE:AD:BE:EF:DE:AD,
// BSSID, seq-ctl, timestamp, interval, capability
static const uint8_t kBeaconHeader[36] = {
    0x80, 0x00,
    0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xde, 0xad, 0xbe, 0xef, 0xde, 0xad,
    0xa1, 0x00, 0x64, 0xe6, 0x0b, 0x8b,
    0x40, 0x43,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x64, 0x00,
    0x11, 0x04
};

static String s_identity;
static String s_name;
static String s_sessionId;
static uint32_t s_pwndTotal = 0;

// Values updated by pentagotchi_grid_update()
static uint32_t s_uptime = 0;
static uint32_t s_pwndSession = 0;
static String s_face;

static bool isGridMac(const uint8_t *mac) {
    return memcmp(mac, kGridMac, sizeof(kGridMac)) == 0;
}

const char *pentagotchi_grid_identity(void) {
    return s_identity.c_str();
}

void pentagotchi_grid_init(const char *name, uint32_t pwnd_total) {
    s_name = name ? name : "pentagotchi";
    s_pwndTotal = pwnd_total;

    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) == ESP_OK) {
        char buf[64];
        size_t len = sizeof(buf);
        if (nvs_get_str(handle, kNvsIdentity, buf, &len) == ESP_OK && len > 0) {
            s_identity = buf;
        } else {
            unsigned char raw[16];
            esp_fill_random(raw, sizeof(raw));
            char hex[sizeof(raw) * 2 + 1];
            for (size_t i = 0; i < sizeof(raw); ++i) {
                snprintf(hex + i * 2, 3, "%02X", raw[i]);
            }
            hex[sizeof(hex) - 1] = '\0';
            s_identity = hex;
            if (nvs_set_str(handle, kNvsIdentity, hex) != ESP_OK || nvs_commit(handle) != ESP_OK) {
                ESP_LOGW(TAG, "Failed to persist identity");
            }
        }
        nvs_close(handle);
    } else {
        ESP_LOGW(TAG, "Failed to open NVS for identity");
    }

    ESP_LOGI(TAG, "Identity: %s (name=%s)", s_identity.c_str(), s_name.c_str());
    SERIAL_PRINTF("[pentagotchi] grid identity: %s (name=%s)\n", s_identity.c_str(), s_name.c_str());

    // Per-boot session id (changes every reboot, like the original unit)
    unsigned char sraw[6];
    esp_fill_random(sraw, sizeof(sraw));
    char sid[18];
    snprintf(sid, sizeof(sid), "%02x:%02x:%02x:%02x:%02x:%02x", sraw[0], sraw[1], sraw[2], sraw[3], sraw[4], sraw[5]);
    s_sessionId = sid;
}

void pentagotchi_grid_update(uint32_t uptime_sec, uint32_t pwnd_session, uint32_t pwnd_total, const char *face) {
    s_uptime = uptime_sec;
    s_pwndSession = pwnd_session;
    s_pwndTotal = pwnd_total;
    if (face) { s_face = face; }
}

// Pal/pwngrid-compatible advertisement JSON (fields expected by other units)
static JsonDocument buildAdvertisement() {
    JsonDocument doc;
    doc["pal"] = true;
    doc["name"] = s_name;
    doc["face"] = s_face.isEmpty() ? String("(^_^)") : s_face;
    doc["epoch"] = 1;
    doc["grid_version"] = "1.10.3";
    doc["identity"] = s_identity;
    doc["pwnd_run"] = s_pwndSession;
    doc["pwnd_tot"] = s_pwndTotal;
    doc["session_id"] = s_sessionId;
    doc["timestamp"] = 0;
    doc["uptime"] = s_uptime;
    doc["version"] = "1.8.4";
    doc["policy"]["advertise"] = true;
    doc["policy"]["bond_encounters_factor"] = 20000;
    doc["policy"]["bored_num_epochs"] = 0;
    doc["policy"]["sad_num_epochs"] = 0;
    doc["policy"]["excited_num_epochs"] = 9999;
    return doc;
}

esp_err_t pentagotchi_grid_send_beacon(void) {
    JsonDocument doc = buildAdvertisement();
    String jsonStr;
    serializeJson(doc, jsonStr);
    const size_t jsonLen = jsonStr.length();
    if (jsonLen == 0) { return ESP_ERR_INVALID_ARG; }

    uint8_t frame[768];
    memset(frame, 0, sizeof(frame));
    memcpy(frame, kBeaconHeader, sizeof(kBeaconHeader));

    size_t frameLen = sizeof(kBeaconHeader);
    // One AC (0xde) + length header, then the whole JSON contiguous.
    // We deliberately do NOT repeat 0xde headers every 255 bytes like pal SPAM:
    // sniffers such as WiFi Marauder slice the RAW bytes between the first '{'
    // and the last '}' of the frame, so embedded headers would corrupt the JSON
    // (InvalidInput). pal/Bruce receivers skip non-ASCII bytes and only ever see
    // the JSON payload, so a single leading header is all they need.
    frame[frameLen++] = 0xde;
    frame[frameLen++] = static_cast<uint8_t>(jsonLen < 255 ? jsonLen : 255);
    for (size_t i = 0; i < jsonLen; ++i) {
        frame[frameLen++] = static_cast<uint8_t>(jsonStr[i]);
    }

    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, frame, static_cast<int>(frameLen), false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "grid beacon TX failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGD(TAG, "grid beacon sent (%d bytes)", static_cast<int>(frameLen));
    }
    return err;
}

static void addOrUpdatePeer(const PwngridPeer &peer) {
    const unsigned long now = millis();

    if (gPeersMutex) { xSemaphoreTake(gPeersMutex, portMAX_DELAY); }

    bool existingNotFound = true;
    for (auto &existing : gPeers) {
        if (existing.identity == peer.identity) {
            const bool newEncounter = (now - existing.lastEncounter) > kEncounterTimeoutMs;
            if (newEncounter) {
                ++existing.encounters;
                existing.lastEncounter = now;
            }
            existing.name = peer.name;
            existing.face = peer.face;
            existing.rssi = peer.rssi;
            existing.uptime = peer.uptime;
            existing.pwndSession = peer.pwndSession;
            existing.pwndTotal = peer.pwndTotal;
            existing.lastPing = now;
            existing.gone = false;

            if (newEncounter) {
                SERIAL_PRINTF("[pentagotchi] peer %s encounter #%lu (%d dBm)\n", existing.identity.c_str(),
                              (unsigned long)existing.encounters, existing.rssi);

                String who = existing.name.isEmpty() ? existing.identity : existing.name;
                pwn_event_t ev = {};
                ev.str = who.c_str();
                ev.value = existing.encounters;
                ev.rssi = existing.rssi;
                pwn_events_raise(PWN_EVENT_PEER_ENCOUNTER, &ev);

                if (existing.encounters == kFriendEncounters) {
                    pwn_events_raise(PWN_EVENT_FRIEND, &ev);
                }
            }
            existingNotFound = false;
            break;
        }
    }

    if (existingNotFound && gPeers.size() < kMaxPeers) {
        PwngridPeer p = peer;
        p.encounters = 1;
        p.lastEncounter = now;
        p.lastPing = now;
        gPeers.push_back(p);
        gTotalFriends = gPeers.size();
        gLastFriendName = p.name;

        ESP_LOGI(TAG, "new peer detected: %s@%s (%d dBm)", p.name.c_str(), p.identity.c_str(), p.rssi);
        SERIAL_PRINTF("[pentagotchi] NEW PEER %s@%s (%d dBm)\n", p.name.c_str(), p.identity.c_str(), p.rssi);

        String who = p.name.isEmpty() ? p.identity : p.name;
        pwn_event_t ev = {};
        ev.str = who.c_str();
        ev.rssi = p.rssi;
        pwn_events_raise(PWN_EVENT_PEER_DETECTED, &ev);
    }

    if (gPeersMutex) { xSemaphoreGive(gPeersMutex); }
}

bool pentagotchi_grid_handle_mgmt(const wifi_promiscuous_pkt_t *packet) {
    if (!packet) { return false; }

    const uint8_t *frame = packet->payload;
    const int len = packet->rx_ctrl.sig_len - 4;

    if (len < 38) { return false; }
    if (!isGridMac(frame + 10)) { return false; }

    const uint16_t frameCtrl = static_cast<uint16_t>(frame[0]) | (static_cast<uint16_t>(frame[1]) << 8);
    const uint8_t subtype = (frameCtrl & 0xF0) >> 4;
    if (subtype != 0x08) { return false; } // only pal/pwngrid beacon advertisements

    // Pal beacons: printable JSON from offset 38 (AC headers are skipped as non-ASCII)
    String payload;
    for (int i = 38; i < len; ++i) {
        uint8_t c = frame[i];
        if (c >= 32 && c <= 126) { payload += static_cast<char>(c); }
    }

    if (payload.isEmpty()) { return true; }

    JsonDocument doc;
    if (deserializeJson(doc, payload) != DeserializationError::Ok) { return true; }

    PwngridPeer peer;
    peer.identity = doc["identity"].as<String>();
    peer.name = doc["name"].as<String>();
    peer.face = doc["face"].as<String>();
    peer.uptime = doc["uptime"] | 0u;
    peer.pwndSession = doc["pwnd_run"] | 0u;
    peer.pwndTotal = doc["pwnd_tot"] | 0u;
    peer.rssi = packet->rx_ctrl.rssi;
    peer.lastPing = millis();

    if (peer.identity.isEmpty() || (!s_identity.isEmpty() && peer.identity == s_identity)) { return true; }

    addOrUpdatePeer(peer);

    if (peer.rssi > gClosestRssi) { gClosestRssi = peer.rssi; }
    return true;
}

void pentagotchi_grid_prune(void) {
    const unsigned long now = millis();

    if (gPeersMutex) { xSemaphoreTake(gPeersMutex, portMAX_DELAY); }

    for (auto it = gPeers.begin(); it != gPeers.end();) {
        if ((now - it->lastPing) > kPeerTimeoutMs || it->gone) {
            SERIAL_PRINTF("[pentagotchi] LOST PEER %s@%s\n", it->name.c_str(), it->identity.c_str());

            String who = it->name.isEmpty() ? it->identity : it->name;
            pwn_event_t ev = {};
            ev.str = who.c_str();
            pwn_events_raise(PWN_EVENT_PEER_GONE, &ev);

            it = gPeers.erase(it);
        } else {
            ++it;
        }
    }
    gTotalFriends = gPeers.size();

    if (gPeersMutex) { xSemaphoreGive(gPeersMutex); }
}

bool pentagotchi_grid_closest_peer(String &face, String &name, uint32_t &pwnd_session,
                                   uint32_t &pwnd_total, int &rssi) {
    const PwngridPeer *closest = nullptr;

    if (gPeersMutex) { xSemaphoreTake(gPeersMutex, portMAX_DELAY); }
    for (const auto &peer : gPeers) {
        if (!closest || peer.rssi > closest->rssi) { closest = &peer; }
    }
    const PwngridPeer copy = closest ? *closest : PwngridPeer{};
    const bool found = closest != nullptr;
    if (gPeersMutex) { xSemaphoreGive(gPeersMutex); }

    if (!found) { return false; }

    face = copy.face;
    name = copy.name;
    pwnd_session = copy.pwndSession;
    pwnd_total = copy.pwndTotal;
    rssi = copy.rssi;
    return true;
}