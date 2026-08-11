#pragma once

#include <Arduino.h>

#include "pentagotchi_events.h"

// MicroQuickJS based plugin runtime for pentagotchi.
//
// JS plugins live in /plugins/*.js on the SD card and look like their
// pwnagotchi counterparts:
//
//   var Example = new Plugin({
//       __author__: 'pentagotchi',
//       __version__: '0.0.1',
//       __license__: 'GPL3',
//       __description__: 'example plugin',
//       on_ui_update: function(ui) { ... },
//       on_handshake: function(agent, filename, ap, client) { ... },
//   });
//
// Supported hooks (see pentagotchi_events.h for the firmware events they map
// to):
//   on_loaded(agent)              loaded, agent == undefined
//   on_ui_setup(ui)               UI layout init, once
//   on_ui_update(ui)              periodic UI refresh (kUiRefreshMs)
//   on_ready(agent)               PWN_EVENT_BOOT
//   on_epoch(agent, epoch)        PWN_EVENT_SCAN_CYCLE
//   on_channel_hop(agent, channel) PWN_EVENT_CHANNEL_CHANGED
//   on_ap_detected(agent, ap)     PWN_EVENT_AP_DETECTED
//   on_wifi_update(agent, aps, clients)
//   on_handshake(agent, filename, ap, client)  PWN_EVENT_HANDSHAKE
//   on_deauthentication(agent, ap, client)     PWN_EVENT_DEAUTH_SENT
//   on_peer_detected(agent, peer)              PWN_EVENT_PEER_DETECTED
//   on_peer_encounter(agent, peer)             PWN_EVENT_PEER_ENCOUNTER
//   on_peer_lost(agent, peer)                  PWN_EVENT_PEER_GONE
//   on_friend(agent, peer)                     PWN_EVENT_FRIEND
//
// Globals available to plugin scripts:
//   agent.config, agent.identity, agent.epoch
//   agent.set_face(s), agent.set_status(s), agent.set_mode(s)
//   ui.set(key, value), ui.get(key), ui.status
//   faces.AWAKE ... faces.LOOK_R_HAPPY  (face palette, also usable with ui.set)
//   Plugin({...}) , setTimeout / clearTimeout, JSON, Math, Date
//
// All dispatch happens from the main loop (poll()), never from the WiFi RX
// ISR: plugin hooks must not block the radio callback.

class PentagotchiPlugins {
public:
    PentagotchiPlugins() = default;

    // Create the JS context after storage/config/UI/events are up, load the
    // /plugins/*.js scripts and bind the firmware event bus. Safe no-op if
    // the runtime failed to initialize.
    void begin();

    // Drive timers, queued firmware events and the periodic on_ui_update.
    // Call from the main loop.
    void poll();

    bool loaded() const { return ctx_ != nullptr; }
    void unloadAll();

private:
    void *heap_ = nullptr;
    void *ctx_ = nullptr;
    unsigned char heapAllocated_ = 0;
};

// Global plugin runtime; created in main.cpp, begin()/poll() called by the
// main task.
extern PentagotchiPlugins gPlugins;