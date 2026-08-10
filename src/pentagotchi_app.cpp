#include "pentagotchi_app.h"

#include "eink_display.h"
#include "pentagotchi_events.h"
#include "pentagotchi_internal.h"

#include <esp_log.h>
#include <esp_random.h>

using namespace pentagotchi::detail;

namespace {

// "PWND" line value: "N (M)" plus " [Name]" when we captured the last handshake.
// Capped so it does not collide with the MODE column on the screen.
void buildShakesLine(char *out, size_t outLen, int session, unsigned long total, const char *attack) {
    char base[24];
    snprintf(base, sizeof(base), "%d (%lu)", session, total);
    const size_t baseLen = strlen(base);
    const size_t kMaxLine = 30;

    size_t nameCap = 0;
    if (attack && attack[0]) {
        const size_t room = (kMaxLine > baseLen + 3) ? (kMaxLine - baseLen - 3) : 0;
        nameCap = room < 32 ? room : 32;
    }

    if (nameCap == 0) {
        snprintf(out, outLen, "%s", base);
        return;
    }

    char name[33];
    const size_t copy = strlen(attack) < nameCap ? strlen(attack) : nameCap;
    memcpy(name, attack, copy);
    name[copy] = '\0';

    snprintf(out, outLen, "%s [%s]", base, name);
}

} // namespace

PentagotchiApp::PentagotchiApp(EInkDisplay &d) : display(d) {}

void PentagotchiApp::begin() {
    gInstance = this;

    gPeersMutex = xSemaphoreCreateMutex();
    pwn_events_init();

    display.begin();

    ensureStorageReady();

    // Config: defaults from code, optional override from /config.json on SD
    pentagotchi_config_set_defaults(&config);
    if (storageReady && !pentagotchi_config_load(&config, true)) {
        pentagotchi_config_save(&config, true); // create a default config file
    }
    gSerialEnabled = config.serial;
    esp_log_level_set("*", config.serial ? ESP_LOG_INFO : ESP_LOG_NONE);
    SERIAL_PRINTF("[pentagotchi] config: name=\"%s\" lang=%s deauth=%d rotation=%s serial=%d\n",
                  config.name, config.lang, config.deauth_enabled, config.display.rotation,
                  config.serial);

    // Display rotation from config (new")/inverted -> 180)
    if (strncmp(config.display.rotation, "inverted", 8) == 0 ||
        strncmp(config.display.rotation, "left", 4) == 0) {
        eink_set_rotation(180);
    } else {
        eink_set_rotation(0);
    }

    // Invert colors when ui.display.color = black
    eink_set_invert(strncmp(config.display.color, "black", 5) == 0);

    randomSeed(esp_random());

    pentagotchi_stats_load(&stats);
    pentagotchi_grid_init(config.name, stats.total_pwnd);
    pwn_ui_init();
    pwn_ui_bind_events();
    eink_set_full_refresh_interval(kFullRefreshIntervalS);
    pwn_ui_set_name(config.name);
    pwn_ui_on_starting();
    pwn_ui_commit();

    deauthEnabled = config.deauth_enabled;

    initWifi();
    pwn_events_raise_simple(PWN_EVENT_BOOT);
    lastCycleTs = millis();
    startTime = millis();
}

void PentagotchiApp::loop() {
    const uint32_t now = millis();

    handleSerialCommands();

    if (handshakePending) {
        handshakePending = false;
        char shakes[PWN_STR_LEN];
        buildShakesLine(shakes, sizeof(shakes), gHandshakeCount, (unsigned long)stats.total_pwnd,
                        gLastPwndName.c_str());
        pwn_ui_set_shakes(shakes);

        pwn_event_t ev = {};
        ev.value = gHandshakeCount;
        pwn_events_raise(PWN_EVENT_HANDSHAKE, &ev);
    }

    if (now - lastCycleTs > kScanCycleMs) {
        rotateChannel();

        uint32_t elapsedSec = (millis() - startTime) / 1000;
        pentagotchi_grid_update(elapsedSec, gHandshakeCount, stats.total_pwnd, pwn_ui_get_face());
        pentagotchi_grid_send_beacon();
        pentagotchi_grid_prune();

        updatePwnUiData();
        if (deauthEnabled) { performDeauthCycle(); }

        pwn_event_t ev = {};
        ev.value = readWifiChannel();
        pwn_events_raise(PWN_EVENT_SCAN_CYCLE, &ev);

        lastCycleTs = now;
    }

    static uint32_t lastUptimeTs = 0;
    if (now - lastUptimeTs >= 1000) {
        updateUptime();
        lastUptimeTs = now;
    }

    static uint32_t lastUi = 0;
    if (now - lastUi > kUiRefreshMs) {
        updateUi(false);
        lastUi = now;
    }

    // Throttled NVS flush of persistent counters (avoids flash wear)
    if (now - lastStatsFlush >= kStatsSaveIntervalMs) {
        if (statsChanges != 0) {
            pentagotchi_stats_save(&stats);
            statsChanges = 0;
        }
        lastStatsFlush = now;
    }
}

void PentagotchiApp::handleSerialCommands() {
    static char line[64];
    static size_t len = 0;

    while (Serial.available() > 0) {
        char c = static_cast<char>(Serial.read());
        if (c == '\n' || c == '\r') {
            if (len == 0) { continue; }
            line[len] = '\0';

            // Trim leading/trailing whitespace
            char *start = line;
            char *end = line + len - 1;
            while (start < end && (*start == ' ' || *start == '\t')) { ++start; }
            while (end > start && (*end == ' ' || *end == '\t')) { --end; }
            end[1] = '\0';

            if (strcmp(start, "clearstats") == 0) {
                stats.total_aps = 0;
                stats.total_pwnd = 0;
                statsChanges = 0;
                pentagotchi_stats_save(&stats);
                Serial.println("> stats cleared");
                SERIAL_PRINTLN("[pentagotchi] stats cleared");
                pwn_events_raise_simple(PWN_EVENT_STATS_CLEARED);
            } else if (strcmp(start, "events") == 0) {
                for (int e = 0; e < PWN_EVENT_COUNT; ++e) {
                    Serial.printf(">  %-16s fired=%lu\n", pwn_events_name(static_cast<pwn_event_id_t>(e)),
                                  (unsigned long)pwn_events_fired(static_cast<pwn_event_id_t>(e)));
                }
                size_t handlers = pwn_events_handler_count();
                Serial.printf(">  %u handler(s):\n", static_cast<unsigned>(handlers));
                for (uint32_t i = 0; i < handlers; ++i) {
                    Serial.printf(">    %-16s -> %s\n", pwn_events_name(pwn_events_handler_id(i)),
                                  pwn_events_handler_tag(i));
                }
            } else {
                Serial.printf("> unknown command: %s\n", start);
            }
            len = 0;
        } else if (len < sizeof(line) - 1) {
            line[len++] = c;
        }
    }
}

void PentagotchiApp::updateUptime() {
    uint32_t elapsed = (millis() - startTime) / 1000;
    uint32_t hh = elapsed / 3600;
    uint32_t mm = (elapsed % 3600) / 60;
    uint32_t ss = elapsed % 60;
    char uptime[16];
    snprintf(uptime, sizeof(uptime), "%02u:%02u:%02u", hh, mm, ss);
    pwn_ui_set_uptime(uptime);
}

void PentagotchiApp::updatePwnUiData() {
    uint8_t ch = readWifiChannel();
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", ch);
    pwn_ui_set_channel(buf);

    size_t currentChannelAps = 0;
    portENTER_CRITICAL(&gRadioMux);
    for (const auto &entry : gRegisteredBeacons) {
        if (entry.channel == ch) { ++currentChannelAps; }
    }
    portEXIT_CRITICAL(&gRadioMux);

    char aps[24];
    snprintf(aps, sizeof(aps), "%zu (%lu)", currentChannelAps, (unsigned long)stats.total_aps);
    pwn_ui_set_aps(aps);

    uint32_t elapsed = (millis() - startTime) / 1000;
    uint32_t hh = elapsed / 3600;
    uint32_t mm = (elapsed % 3600) / 60;
    uint32_t ss = elapsed % 60;
    char uptime[16];
    snprintf(uptime, sizeof(uptime), "%02u:%02u:%02u", hh, mm, ss);
    pwn_ui_set_uptime(uptime);

    char shakes[PWN_STR_LEN];
    buildShakesLine(shakes, sizeof(shakes), gHandshakeCount, (unsigned long)stats.total_pwnd,
                    gLastPwndName.c_str());
    pwn_ui_set_shakes(shakes);

    String closestFace, closestName;
    uint32_t closestSession = 0, closestTotal = 0;
    int closestRssi = -1000;
    if (pentagotchi_grid_closest_peer(closestFace, closestName, closestSession, closestTotal, closestRssi)) {
        char friendBuf[PWN_FRIEND_NAME_LEN];
        if (!closestName.isEmpty()) {
            snprintf(friendBuf, sizeof(friendBuf), "%s %lu (%lu)", closestName.c_str(),
                     (unsigned long)closestSession, (unsigned long)closestTotal);
        } else {
            snprintf(friendBuf, sizeof(friendBuf), "%lu (%lu)", (unsigned long)closestSession,
                     (unsigned long)closestTotal);
        }
        pwn_ui_set_friend(closestFace.isEmpty() ? PWN_FACE_FRIEND : closestFace.c_str(), friendBuf, closestRssi);
    } else {
        pwn_ui_set_friend(nullptr, nullptr, -1000);
    }
}
