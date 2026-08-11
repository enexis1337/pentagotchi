#include "pentagotchi_events.h"

#include <Arduino.h>
#include <cstdio>
#include <cstring>

#define PWN_EVENT_MAX_HANDLERS 32

typedef struct {
    bool active;
    pwn_event_id_t id;
    pwn_event_handler cb;
    char tag[16];
} handler_slot_t;

static handler_slot_t s_handlers[PWN_EVENT_MAX_HANDLERS];
static uint32_t s_fired[PWN_EVENT_COUNT];

static const char *const kEventNames[PWN_EVENT_COUNT] = {
    "boot",
    "scan_cycle",
    "channel_changed",
    "ap_detected",
    "handshake",
    "deauth_sent",
    "peer_detected",
    "peer_encounter",
    "peer_gone",
    "friend",
    "mood_changed",
    "cooldown",
    "stats_cleared",
};

void pwn_events_init(void) {
    memset(s_handlers, 0, sizeof(s_handlers));
    memset(s_fired, 0, sizeof(s_fired));
}

bool pwn_events_subscribe(pwn_event_id_t id, pwn_event_handler cb, const char *tag) {
    if (!cb || id >= PWN_EVENT_COUNT) { return false; }

    for (size_t i = 0; i < PWN_EVENT_MAX_HANDLERS; ++i) {
        if (!s_handlers[i].active) {
            s_handlers[i].active = true;
            s_handlers[i].id = id;
            s_handlers[i].cb = cb;
            snprintf(s_handlers[i].tag, sizeof(s_handlers[i].tag), "%s", tag ? tag : "?");
            return true;
        }
    }
    return false;
}

void pwn_events_unsubscribe(pwn_event_id_t id, pwn_event_handler cb) {
    for (size_t i = 0; i < PWN_EVENT_MAX_HANDLERS; ++i) {
        if (s_handlers[i].active && s_handlers[i].id == id && s_handlers[i].cb == cb) {
            s_handlers[i].active = false;
            s_handlers[i].cb = nullptr;
        }
    }
}

void pwn_events_raise(pwn_event_id_t id, const pwn_event_t *params) {
    if (id >= PWN_EVENT_COUNT) { return; }
    s_fired[id]++;

    pwn_event_t ev;
    if (params) {
        ev = *params;
    } else {
        memset(&ev, 0, sizeof(ev));
    }
    ev.id = id;
    ev.ts = millis();

    for (size_t i = 0; i < PWN_EVENT_MAX_HANDLERS; ++i) {
        if (s_handlers[i].active && s_handlers[i].id == id && s_handlers[i].cb) {
            s_handlers[i].cb(&ev);
        }
    }
}

void pwn_events_raise_simple(pwn_event_id_t id) {
    pwn_events_raise(id, nullptr);
}

const char *pwn_events_name(pwn_event_id_t id) {
    return (id < PWN_EVENT_COUNT) ? kEventNames[id] : "?";
}

uint32_t pwn_events_fired(pwn_event_id_t id) {
    return (id < PWN_EVENT_COUNT) ? s_fired[id] : 0;
}

size_t pwn_events_handler_count(void) {
    size_t n = 0;
    for (size_t i = 0; i < PWN_EVENT_MAX_HANDLERS; ++i) {
        if (s_handlers[i].active) { ++n; }
    }
    return n;
}

const char *pwn_events_handler_tag(uint32_t index) {
    for (size_t i = 0, seen = 0; i < PWN_EVENT_MAX_HANDLERS; ++i) {
        if (s_handlers[i].active) {
            if (seen == index) { return s_handlers[i].tag; }
            ++seen;
        }
    }
    return nullptr;
}

pwn_event_id_t pwn_events_handler_id(uint32_t index) {
    for (size_t i = 0, seen = 0; i < PWN_EVENT_MAX_HANDLERS; ++i) {
        if (s_handlers[i].active) {
            if (seen == index) { return s_handlers[i].id; }
            ++seen;
        }
    }
    return PWN_EVENT_COUNT;
}