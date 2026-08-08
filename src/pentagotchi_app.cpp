#include "pwnagotchi_app.h"

#include "eink_display.h"
#include "pwnagotchi_internal.h"

#include <esp_log.h>
#include <esp_random.h>

using namespace pwnagotchi::detail;

PwnagotchiApp::PwnagotchiApp(EInkDisplay &d) : display(d) {}

void PwnagotchiApp::begin() {
    gInstance = this;

    display.begin();

    ensureStorageReady();

    // Config: defaults from code, optional override from /config.json on SD
    pwnagotchi_config_set_defaults(&config);
    if (storageReady && !pwnagotchi_config_load(&config, true)) {
        pwnagotchi_config_save(&config, true); // create a default config file
    }
    Serial.printf("[pwnagotchi] config: name=\"%s\" lang=%s deauth=%d rotation=%s\n",
                  config.name, config.lang, config.deauth_enabled, config.display.rotation);

    // Display rotation from config (new")/inverted -> 180)
    if (strncmp(config.display.rotation, "inverted", 8) == 0 ||
        strncmp(config.display.rotation, "left", 4) == 0) {
        eink_set_rotation(180);
    } else {
        eink_set_rotation(0);
    }

    randomSeed(esp_random());

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

void PwnagotchiApp::loop() {
    const uint32_t now = millis();

    if (handshakePending) {
        handshakePending = false;
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
}

void PwnagotchiApp::updateUptime() {
    uint32_t elapsed = (millis() - startTime) / 1000;
    uint32_t hh = elapsed / 3600;
    uint32_t mm = (elapsed % 3600) / 60;
    uint32_t ss = elapsed % 60;
    char uptime[16];
    snprintf(uptime, sizeof(uptime), "%02u:%02u:%02u", hh, mm, ss);
    pwn_ui_set_uptime(uptime);
}

void PwnagotchiApp::updatePwnUiData() {
    uint8_t ch = readWifiChannel();
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", ch);
    pwn_ui_set_channel(buf);

    char aps[24];
    snprintf(aps, sizeof(aps), "%zu (%u)", gRegisteredBeacons.size(), gTotalFriends);
    pwn_ui_set_aps(aps);

    uint32_t elapsed = (millis() - startTime) / 1000;
    uint32_t hh = elapsed / 3600;
    uint32_t mm = (elapsed % 3600) / 60;
    uint32_t ss = elapsed % 60;
    char uptime[16];
    snprintf(uptime, sizeof(uptime), "%02u:%02u:%02u", hh, mm, ss);
    pwn_ui_set_uptime(uptime);

    char shakes[16];
    snprintf(shakes, sizeof(shakes), "%d (%d)", gHandshakeCount, gHandshakeCount);
    pwn_ui_set_shakes(shakes);

    if (!gLastFriendName.isEmpty() && gTotalFriends > 0) {
        pwn_ui_set_friend(PWN_FACE_FRIEND, gLastFriendName.c_str());
    } else {
        pwn_ui_set_friend(nullptr, nullptr);
    }
}

void PwnagotchiApp::triggerRandomMood() {
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

void PwnagotchiApp::wakeAnimation() {
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
