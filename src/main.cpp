#include <Arduino.h>

#include "eink_display.h"
#include "pentagotchi_app.h"
#include "pentagotchi_internal.h"

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
