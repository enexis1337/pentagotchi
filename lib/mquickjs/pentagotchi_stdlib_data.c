/*
 * pentagotchi_stdlib_data.c
 *
 * Defines the MicroQuickJS standard library ROM tables for the pentagotchi
 * plugin runtime.
 *
 * The tables are generated (pentagotchi_stdlib.h) from pentagotchi_stdlib.c
 * by tools/mqjs-stdlib/; they are compiled here as C so the 32-bit ROM value
 * layout stays intact (C++ would complain about narrowing initializers).
 */

#include <stddef.h>
#include "../../include/pentagotchi_plugins_api.h"
#include "pentagotchi_stdlib.h"