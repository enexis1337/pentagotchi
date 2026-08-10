#include <Arduino.h>

#include "eink_display.h"
#include "pentagotchi_app.h"
#include "pentagotchi_internal.h"

// Single source of truth for the firmware version (see kFirmwareVersion in
// pentagotchi_config.h). Stored as "main.version" in /config.json; when the
// stored config version differs, pentagotchi_config_load() re-saves the
// config with this version, keeping the user's settings intact.
const char kFirmwareVersion[] = "0.1.0";

EInkDisplay display;
PentagotchiApp app(display);

void setup() {
    Serial.begin(115200);
    delay(200);
    SERIAL_PRINTLN("[main] boot begin");
    app.begin();
    SERIAL_PRINTLN("[main] app.begin done");
}

void loop() {
    app.loop();
}
