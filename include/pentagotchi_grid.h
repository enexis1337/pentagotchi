#pragma once

#include <Arduino.h>
#include <esp_wifi_types.h>

// Pwngrid mesh: persistent identity, beacon advertisement TX/RX,
// peer tracking with encounters and expiry.
//
// Beacons use the pal/pwngrid-compatible format shared with Bruce/PwnGridSpam
// units and the original pwnagotchi ecosystem: spoofed MAC DE:AD:BE:EF:DE:AD,
// one AC (0xde + len) header then the full advertisement JSON as printable
// ASCII after offset 38. The JSON is sent contiguous (no per-255-byte AC
// headers) so WiFi Marauder, which slices the raw bytes between the first '{'
// and the last '}', can parse it; pal/Bruce receivers skip non-ASCII bytes.

#ifdef __cplusplus
extern "C" {
#endif

// Load (or generate and persist) the unit identity, cache unit name.
void pentagotchi_grid_init(const char *name, uint32_t pwnd_total);

// Enable/disable the mesh (guard for all grid operations). Disabled = no
// beacons sent, no peers tracked, incoming grid frames ignored but still
// excluded from the AP counter.
void pentagotchi_grid_set_enabled(bool enabled);

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