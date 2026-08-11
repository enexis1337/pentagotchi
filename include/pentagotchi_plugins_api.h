#pragma once
/**
 * pentagotchi_plugins_api.h
 *
 * C-compatible bindings shared between the MicroQuickJS standard library
 * (lib/mquickjs/pentagotchi_stdlib.c, which is only used to GENERATE the
 * stdlib ROM tables) and the host runtime (src/pentagotchi_plugins.cpp,
 * which IMPLEMENTS every symbol referenced by the generated table).
 *
 * All symbols here must have "C" linkage so they resolve from the generated
 * stdlib tables.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "mquickjs.h"

/* ------------------------------------------------------------------ */
/* Engine "REPL" glue — host implementations of the stdlib C functions */
/* (origins: mqjs.c from the reference REPL)                           */
/* ------------------------------------------------------------------ */

JSValue js_print(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_gc(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_load(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_setTimeout(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_clearTimeout(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_date_constructor(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_date_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_performance_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

/* ------------------------------------------------------------------ */
/* Pentagotchi plugin runtime                                         */
/*                                                                     */
/* Global JS API (kept close to the original pwnagotchi plugins):      */
/*                                                                     */
/*   var Foo = new Plugin({ __author__, __version__, __license__,      */
/*                          __description__, on_loaded, on_handshake,  */
/*                          on_channel_hop, on_ui_update, ... });      */
/*                                                                     */
/*   agent.config       -> frozen snapshot of /config.json             */
/*   agent.set_face()   agent.set_status()  agent.set_mode()           */
/*   ui.set(key,value)  ui.get(key)                                    */
/*   faces.AWAKE ...    face constants (same as pwn_ui.h)              */
/* ------------------------------------------------------------------ */

/* Plugin constructor: new Plugin({...}) registers the instance. */
JSValue js_pwn_plugin(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

/* agent object */
JSValue js_pwn_agent_set_face(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_pwn_agent_set_status(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_pwn_agent_set_mode(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_pwn_agent_run(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_pwn_agent_get_config(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_pwn_agent_get_identity(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_pwn_agent_get_epoch(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

/* ui object */
JSValue js_pwn_ui_set(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_pwn_ui_get(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_pwn_ui_get_status(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_pwn_ui_set_status(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

/* faces object: generic_magic getter, magic = index into the face table */
JSValue js_pwn_face_get(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv, int magic);

/* JSON helpers (implemented by the engine, exposed so the runtime glue can
   serialize/deserialize plugin options objects to the /plugins.json file) */
JSValue js_json_parse(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
JSValue js_json_stringify(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

#ifdef __cplusplus
}
#endif