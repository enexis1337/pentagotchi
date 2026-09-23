#pragma once

#include "pentagotchi_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// HTTP web UI served over the SoftAP created by initWifi() when
// config.ui.web.enabled is true (default: off). Compact single-page status UI
// in the spirit of the original pwnagotchi web UI, with basic auth from
// ui.web.username/password and a /api/status JSON endpoint.
void pentagotchi_web_start(const pentagotchi_config_t *cfg);
void pentagotchi_web_stop(void);
bool pentagotchi_web_is_running(void);

#ifdef __cplusplus
}
#endif
