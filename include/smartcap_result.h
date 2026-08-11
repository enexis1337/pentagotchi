#pragma once

// Result-capture integration point for SmartCap. The FSM calls these when a
// full 4-way handshake or a PMKID has been captured; a consumer registers one
// function per event type. Until one is registered the default behavior is to
// only log (dry-run friendly). The firmware will register hooks that write
// pcap/GPS captures later.

#include "smartcap_types.h"

#include <stdbool.h>
#include <stdint.h>

typedef void (*smartcap_result_fn)(const smartcap_target_t *target, uint32_t now_ms);

// Install the capture sink (or clear with a NULL).
void smartcap_result_set_handshake_fn(smartcap_result_fn fn);
void smartcap_result_set_pmkid_fn(smartcap_result_fn fn);

// Called by the FSM once a target is about to be marked closed. Safe to call
// repeatedly - the sink should be idempotent (or the FSM must guarantee it).
void smartcap_result_handshake(const smartcap_target_t *target, uint32_t now_ms);
void smartcap_result_pmkid(const smartcap_target_t *target, uint32_t now_ms);

// Optional debug logger (same pattern as smartcap_radio).
typedef void (*smartcap_result_log_fn)(const char *line);
void smartcap_result_set_logger(smartcap_result_log_fn fn);