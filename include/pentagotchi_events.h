#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Event bus modeled after the original pwnagotchi's plugin hooks (on_*).
// Code that DETECTS something calls pwn_events_raise(); interested modules
// (UI, stats, AI, grid, ...) subscribe handlers and react, without the
// sources having to know about them. Handlers MUST be lightweight: events may
// be raised from the promiscuous RX context.

typedef enum {
    PWN_EVENT_BOOT = 0,          // unit powered, main task starting
    PWN_EVENT_SCAN_CYCLE,        // one scan+advertise cycle done (value = channel)
    PWN_EVENT_CHANNEL_CHANGED,   // radio switched channel (value = new channel)
    PWN_EVENT_AP_DETECTED,       // new unique AP seen (value = channel, mac = AP)
    PWN_EVENT_HANDSHAKE,         // new 4-way handshake captured
    PWN_EVENT_DEAUTH_SENT,       // deauth attack frame sent (mac = target, str = MAC string)
    PWN_EVENT_PEER_DETECTED,     // new grid peer first seen (str = name/identity)
    PWN_EVENT_PEER_ENCOUNTER,    // known peer seen again (value = encounter count)
    PWN_EVENT_PEER_GONE,         // grid peer pruned/timed out (str = name/identity)
    PWN_EVENT_FRIEND,            // peer crossed the friend threshold
    PWN_EVENT_MOOD_CHANGED,      // mood switched (value = pwn_mood_t)
    PWN_EVENT_COOLDOWN,          // FSM COOLDOWN entered (value = cooldown duration ms)
    PWN_EVENT_STATS_CLEARED,     // cumulative counters reset via CLI
    PWN_EVENT_COUNT
} pwn_event_id_t;

typedef enum {
    PWN_MOOD_NORMAL = 0,
    PWN_MOOD_BORED,
    PWN_MOOD_SAD,
    PWN_MOOD_LONELY,
    PWN_MOOD_EXCITED,
    PWN_MOOD_MOTIVATED,
    PWN_MOOD_COUNT
} pwn_mood_t;

typedef struct {
    pwn_event_id_t id;      // which event fired
    uint32_t ts;            // millis() at raise time
    int32_t value;          // generic int payload (mood, encounter #, channel, ...)
    int32_t rssi;           // signal strength in dBm when applicable
    const uint8_t *mac;     // 6-byte MAC when applicable (may be NULL, valid during raise)
    const char *str;        // short string payload when applicable (may be NULL)
} pwn_event_t;

typedef void (*pwn_event_handler)(const pwn_event_t *ev);

#ifdef __cplusplus
extern "C" {
#endif

void     pwn_events_init(void);

// Register cb for event id. tag is a human name used by the "events" CLI.
// Returns false if the table is full. Must be called before events fire
// (i.e. during begin()); raising from the RX callback is safe afterwards.
bool     pwn_events_subscribe(pwn_event_id_t id, pwn_event_handler cb, const char *tag);

void     pwn_events_unsubscribe(pwn_event_id_t id, pwn_event_handler cb);

// Fire an event. params may be NULL ("simple" event); ts/id are always set.
void     pwn_events_raise(pwn_event_id_t id, const pwn_event_t *params);
void     pwn_events_raise_simple(pwn_event_id_t id);

// Diagnostics for the CLI.
const char      *pwn_events_name(pwn_event_id_t id);
uint32_t         pwn_events_fired(pwn_event_id_t id);
size_t           pwn_events_handler_count(void);
const char      *pwn_events_handler_tag(uint32_t index);   // index of a subscribed slot
pwn_event_id_t   pwn_events_handler_id(uint32_t index);

#ifdef __cplusplus
}
#endif