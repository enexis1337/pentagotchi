#pragma once

#include <Arduino.h>
#include <esp_wifi_types.h>

// Pwngrid mesh: persistent identity, beacon advertisement TX/RX,
// peer tracking with encounters and expiry.
//
// Beacons use the pal/pwngrid-compatible format shared with Bruce/PwnGridSpam
// units and the original pwnagotchi ecosystem: spoofed MAC DE:AD:BE:EF:DE:AD,
// the advertisement JSON chunked with 0xde+len "AC" headers, all printable
// ASCII payload after offset 38.

#ifdef __cplusplus
extern "C" {
#endif

// Load (or generate and persist) the unit identity, cache unit name.
void pentagotchi_grid_init(const char *name, uint32_t pwnd_total);

// Update the values embedded in our own advertisement.
void pentagotchi_grid_update(uint32_t uptime_sec, uint32_t pwnd_session, uint32_t pwnd_total, const char *face);

// Transmit our advertisement beacon on the currently set channel.
esp_err_t pentagotchi_grid_send_beacon(void);

// Parse an incoming management frame. Returns true if it was a grid frame
// from another unit (DE:AD:BE:EF:DE:AD) regardless of whether it parsed.
bool pentagotchi_grid_handle_mgmt(const wifi_promiscuous_pkt_t *packet);

// Remove peers that have not been heard of for too long.
void pentagotchi_grid_prune(void);

// Identity fingerprint string (hex).
const char *pentagotchi_grid_identity(void);

// Fill face/name/counters of the currently closest peer. Returns false if no peers.
bool pentagotchi_grid_closest_peer(String &face, String &name, uint32_t &pwnd_session,
                                   uint32_t &pwnd_total, int &rssi);

#ifdef __cplusplus
}
#endif