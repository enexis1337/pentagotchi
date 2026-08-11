#include "pentagotchi_app.h"

#include "eink_display.h"
#include "pentagotchi_internal.h"

using namespace pentagotchi::detail;

void PentagotchiApp::updateUi(bool fullRefresh) {
    pwn_ui_tick(); // periodic maintenance (cooldown sleep-face animation)
    if (fullRefresh || eink_should_do_full_refresh()) {
        pwn_ui_full_commit();
        eink_mark_full_refresh_done();
    } else {
        pwn_ui_commit();
    }
}