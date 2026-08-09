#include "pentagotchi_app.h"

#include "eink_display.h"
#include "pentagotchi_internal.h"

#include <esp_log.h>
#include <esp_random.h>

using namespace pentagotchi::detail;

PentagotchiApp::PentagotchiApp(EInkDisplay &d) : display(d) {}

void PentagotchiApp::begin() {
    gInstance = this;

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
    pwn_ui_init();
    eink_set_full_refresh_interval(kFullRefreshIntervalS);
    pwn_ui_set_name(config.name);
    pwn_ui_on_starting();
    pwn_ui_commit();

    deauthEnabled = config.deauth_enabled;

    initWifi();
    wakeAnimation();
    lastCycleTs = millis();
    lastMoodSwitch = millis();
    randomMoodInterval = kMoodMinMs;
    startTime = millis();
}

void PentagotchiApp::loop() {
    const uint32_t now = millis();

    handleSerialCommands();

    if (handshakePending) {
        handshakePending = false;
        char shakes[16];
        snprintf(shakes, sizeof(shakes), "%d (%lu)", gHandshakeCount, (unsigned long)stats.total_pwnd);
        pwn_ui_set_shakes(shakes);
        pwn_ui_on_handshake();
    }

    if (now - lastMoodSwitch > randomMoodInterval) {
        triggerRandomMood();
        lastMoodSwitch = now;
        randomMoodInterval = kMoodMinMs + (kMoodMaxMs > kMoodMinMs ? esp_random() % (kMoodMaxMs - kMoodMinMs) : 0);
    }

    if (now - lastCycleTs > kScanCycleMs) {
        rotateChannel();
        updatePwnUiData();
        if (deauthEnabled) { performDeauthCycle(); }
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
                pwn_ui_force_update();
                Serial.println("> stats cleared");
                SERIAL_PRINTLN("[pentagotchi] stats cleared");
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

    char shakes[16];
    snprintf(shakes, sizeof(shakes), "%d (%lu)", gHandshakeCount, (unsigned long)stats.total_pwnd);
    pwn_ui_set_shakes(shakes);

    if (!gLastFriendName.isEmpty() && gTotalFriends > 0) {
        pwn_ui_set_friend(PWN_FACE_FRIEND, gLastFriendName.c_str());
    } else {
        pwn_ui_set_friend(nullptr, nullptr);
    }
}

void PentagotchiApp::triggerRandomMood() {
    const int r = esp_random() % 6;
    switch (r) {
        case 0: pwn_ui_on_normal(); break;
        case 1: pwn_ui_on_bored();  break;
        case 2: pwn_ui_on_sad();    break;
        case 3: pwn_ui_on_lonely(); break;
        case 4: pwn_ui_on_excited(); break;
        case 5: pwn_ui_on_motivated(); break;
    }
}

void PentagotchiApp::wakeAnimation() {
    pwn_ui_set_face(PWN_FACE_SLEEP);
    pwn_ui_set_status("Waking up...");
    pwn_ui_full_commit();
    delay(300);

    pwn_ui_set_face(PWN_FACE_AWAKE);
    pwn_ui_set_status("Hello! I'm pentagotchi");
    pwn_ui_full_commit();
    delay(300);

    pwn_ui_on_normal();
    pwn_ui_full_commit();
}
