#include "pentagotchi_gps.h"

#include "pentagotchi_internal.h"
#include "pins.h"

#include <Arduino.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Driver for the Teyleten Robot ATGM336H GPS module: NMEA 0183 sentences
// (GGA/RMC) over UART1 at 9600 8N1. Pins come from pins.h.
//
// The module is receive-only in normal use: we only listen on its TX line
// (ESP RX pin). gps_init() also enables the TX pin in case configuration
// commands are ever needed.

namespace {

gps_fix_t s_fix{};
bool s_uart_ok = false;
bool s_inited = false;

char s_line[128];
size_t s_line_len = 0;

// Copy the idx-th comma-separated field of an NMEA sentence into out.
bool nmea_field(const char *sentence, int idx, char *out, size_t outSize) {
    const char *p = sentence;
    for (int cur = 0; cur < idx; ++cur) {
        p = strchr(p, ',');
        if (!p) { return false; }
        ++p;
    }

    const char *end = strchr(p, ',');
    const char *star = strchr(p, '*');
    size_t n = end ? static_cast<size_t>(end - p) : strlen(p);
    if (star && (!end || star < end)) { n = static_cast<size_t>(star - p); }
    if (n >= outSize) { n = outSize - 1; }
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

// "ddmm.mm" + hemisphere -> decimal degrees; returns NAN on bad input.
double parseNmeaCoord(const char *val, char hemi) {
    if (!val[0] || !hemi) { return NAN; }
    char *end = nullptr;
    double raw = strtod(val, &end);
    if (end == val) { return NAN; }

    int deg = static_cast<int>(raw / 100.0);
    double degs = deg + (raw - static_cast<double>(deg) * 100.0) / 60.0;
    if (hemi == 'S' || hemi == 'W') { degs = -degs; }
    return degs;
}

// Verify the trailing *XX checksum. Sentences without a checksum are accepted.
bool checksumOk(const char *line) {
    const char *star = strchr(line, '*');
    if (!star) { return true; }

    uint8_t sum = 0;
    for (const char *p = line + 1; p < star; ++p) { sum ^= static_cast<uint8_t>(*p); }
    unsigned int hex = 0;
    if (sscanf(star + 1, "%2x", &hex) != 1) { return false; }
    return (sum & 0xFF) == (hex & 0xFF);
}

void setTime(const char *buf) {
    if (strlen(buf) < 6) { return; }
    s_fix.hour = static_cast<uint8_t>((buf[0] - '0') * 10 + (buf[1] - '0'));
    s_fix.minute = static_cast<uint8_t>((buf[2] - '0') * 10 + (buf[3] - '0'));
    s_fix.second = static_cast<uint8_t>((buf[4] - '0') * 10 + (buf[5] - '0'));
}

void revalidate() {
    s_fix.valid = s_fix.quality > 0 && !std::isnan(s_fix.latitude) && !std::isnan(s_fix.longitude);
}

void parseGga(const char *line) {
    char buf[16];

    if (nmea_field(line, 1, buf, sizeof(buf))) { setTime(buf); }

    char lat[16], lon[16], ns[2], ew[2];
    if (nmea_field(line, 2, lat, sizeof(lat)) && nmea_field(line, 3, ns, sizeof(ns)) &&
        nmea_field(line, 4, lon, sizeof(lon)) && nmea_field(line, 5, ew, sizeof(ew))) {
        s_fix.latitude = parseNmeaCoord(lat, ns[0]);
        s_fix.longitude = parseNmeaCoord(lon, ew[0]);
    }

    char qual[4];
    if (nmea_field(line, 6, qual, sizeof(qual)) && qual[0]) {
        s_fix.quality = static_cast<uint8_t>(atoi(qual));
    }

    char sv[8];
    if (nmea_field(line, 7, sv, sizeof(sv)) && sv[0]) {
        s_fix.satellites = static_cast<uint16_t>(atoi(sv));
    }

    char alt[16];
    if (nmea_field(line, 9, alt, sizeof(alt)) && alt[0]) {
        s_fix.altitude = static_cast<float>(atof(alt));
    }

    revalidate();
}

void parseRmc(const char *line) {
    char status[2];
    if (!nmea_field(line, 2, status, sizeof(status)) || status[0] != 'A') { return; }

    char tim[16];
    if (nmea_field(line, 1, tim, sizeof(tim))) { setTime(tim); }

    char lat[16], lon[16], ns[2], ew[2];
    if (nmea_field(line, 3, lat, sizeof(lat)) && nmea_field(line, 4, ns, sizeof(ns)) &&
        nmea_field(line, 5, lon, sizeof(lon)) && nmea_field(line, 6, ew, sizeof(ew))) {
        s_fix.latitude = parseNmeaCoord(lat, ns[0]);
        s_fix.longitude = parseNmeaCoord(lon, ew[0]);
    }

    char spd[16];
    if (nmea_field(line, 7, spd, sizeof(spd)) && spd[0]) {
        s_fix.speed = static_cast<float>(atof(spd)) * 1.852f; // knots -> km/h
    }

    char date[8];
    if (nmea_field(line, 9, date, sizeof(date)) && strlen(date) >= 6) {
        int d = atoi(date); // ddmmyy
        s_fix.day = static_cast<uint8_t>(d / 10000);
        s_fix.month = static_cast<uint8_t>((d / 100) % 100);
        s_fix.year = static_cast<uint16_t>(2000 + (d % 100));
        s_fix.have_date = true;
    }

    revalidate();
}

void processSentence(const char *line) {
    if (line[0] != '$') { return; }
    if (!checksumOk(line)) { return; }

    if (strncmp(line + 3, "GGA", 3) == 0) {
        parseGga(line);
    } else if (strncmp(line + 3, "RMC", 3) == 0) {
        parseRmc(line);
    }
}

} // namespace

bool gps_init(void) {
    if (s_inited) { return s_uart_ok; }
    s_inited = true;

    Serial1.begin(GPS_BAUD_RATE, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
    delay(10);

    s_uart_ok = true;
    ESP_LOGI(kLogTag, "GPS init: UART1 RX=GPIO%d TX=GPIO%d @ %d",
             static_cast<int>(PIN_GPS_RX), static_cast<int>(PIN_GPS_TX), GPS_BAUD_RATE);
    SERIAL_PRINTF("[pentagotchi] GPS init: UART1 RX=GPIO%d TX=GPIO%d @ %d\n",
                  static_cast<int>(PIN_GPS_RX), static_cast<int>(PIN_GPS_TX), GPS_BAUD_RATE);
    return s_uart_ok;
}

void gps_update(void) {
    if (!s_uart_ok) { return; }

    while (Serial1.available() > 0) {
        char c = static_cast<char>(Serial1.read());
        if (c == '\n') {
            s_line[s_line_len] = '\0';
            processSentence(s_line);
            s_line_len = 0;
        } else if (c != '\r' && s_line_len < sizeof(s_line) - 1) {
            s_line[s_line_len++] = c;
        }
    }
}

const gps_fix_t *gps_fix(void) {
    return &s_fix;
}

bool gps_has_fix(void) {
    return s_fix.valid && s_fix.have_date;
}