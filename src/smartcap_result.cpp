// Result-capture bridge for SmartCap. Host-compilable; the default sink logs
// via an optional logger and does nothing else.

#include "smartcap_result.h"

#include <stdio.h>

namespace {
smartcap_result_fn gHandshakeFn = nullptr;
smartcap_result_fn gPmkidFn = nullptr;
smartcap_result_log_fn gLogger = nullptr;

void logLine(const char *line) {
    if (gLogger) {
        gLogger(line);
    }
}
} // namespace

void smartcap_result_set_handshake_fn(smartcap_result_fn fn) { gHandshakeFn = fn; }
void smartcap_result_set_pmkid_fn(smartcap_result_fn fn) { gPmkidFn = fn; }
void smartcap_result_set_logger(smartcap_result_log_fn fn) { gLogger = fn; }

void smartcap_result_handshake(const smartcap_target_t *target, uint32_t now_ms) {
    if (gHandshakeFn) {
        gHandshakeFn(target, now_ms);
        return;
    }
    char line[128];
    snprintf(line, sizeof(line),
             "RESULT: full 4-way handshake captured for %02X:%02X:...:%02X (ch=%u, rssi=%d)",
             target->bssid[0], target->bssid[1], target->bssid[5],
             target->channel, target->rssi);
    logLine(line);
}

void smartcap_result_pmkid(const smartcap_target_t *target, uint32_t now_ms) {
    if (gPmkidFn) {
        gPmkidFn(target, now_ms);
        return;
    }
    char line[128];
    snprintf(line, sizeof(line),
             "RESULT: PMKID captured for %02X:%02X:...:%02X (ch=%u, rssi=%d)",
             target->bssid[0], target->bssid[1], target->bssid[5],
             target->channel, target->rssi);
    logLine(line);
}