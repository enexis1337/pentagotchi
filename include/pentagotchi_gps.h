#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Latest GPS fix from the Teyleten Robot ATGM336H module (NMEA over UART1).
// All numeric values are populated from the most recent valid sentence(s).
typedef struct {
    bool     valid;        // usable fix: quality > 0 and coordinates are finite
    bool     have_date;    // RMC date (day/month/year) received yet
    uint8_t  quality;      // GGA fix quality: 0 = no fix, 1 = GPS, 2 = DGPS, ...
    double   latitude;     // decimal degrees; north positive
    double   longitude;    // decimal degrees; east positive
    float    altitude;     // meters above mean sea level
    float    speed;        // km/h
    uint16_t satellites;   // satellites in use

    // UTC time, from GGA/RMC (hour:min:sec) + RMC date
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
} gps_fix_t;

// Initialize the GPS UART. Returns true on success (no-op if already started).
bool gps_init(void);

// Pump the UART: read bytes and parse one complete NMEA sentence if available.
// Cheap enough to call every loop() iteration.
void gps_update(void);

// Pointer to the current fix state (never NULL).
const gps_fix_t *gps_fix(void);

// True when we have a usable fix AND a full UTC date for the "Updated" field.
bool gps_has_fix(void);

#ifdef __cplusplus
}
#endif