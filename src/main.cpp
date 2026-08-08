#include <Arduino.h>

#include "eink_display.h"
#include "pwnagotchi_app.h"

EInkDisplay display;
PwnagotchiApp app(display);

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("[main] boot begin");
    app.begin();
    Serial.println("[main] app.begin done");
}

void loop() {
    app.loop();
}
