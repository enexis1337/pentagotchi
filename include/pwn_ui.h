#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Display dimensions
#define PWN_UI_W 250
#define PWN_UI_H 122

#define PWN_FACE_LEN       24
#define PWN_NAME_LEN       24
#define PWN_STATUS_LEN     128
#define PWN_STR_LEN        40
#define PWN_FRIEND_NAME_LEN 64

// Layout positions (matching original pentagotchi Waveshare V2/V3 black, 250x122)
#define PWN_X_FACE       0
#define PWN_Y_FACE       40
#define PWN_X_NAME       5
#define PWN_Y_NAME       20
#define PWN_NAME_PROMPT  ">"
#define PWN_X_CH         0
#define PWN_Y_CH         0
#define PWN_X_APS        32
#define PWN_Y_APS        0
#define PWN_X_UPTIME     169
#define PWN_Y_UPTIME     0
#define PWN_X_STATUS     116
#define PWN_Y_STATUS     20
#define PWN_STATUS_MAX   20
#define PWN_X_FRIEND_FACE 0
#define PWN_Y_FRIEND_FACE 92
#define PWN_X_FRIEND_NAME 40
#define PWN_Y_FRIEND_NAME 94
#define PWN_X_SHAKES     0
#define PWN_Y_SHAKES     109
#define PWN_X_MODE       220
#define PWN_Y_MODE       109
#define PWN_LINE1_Y      14
#define PWN_LINE2_Y      107

// Original pwnagotchi faces (UTF-8, rendered by DejaVuSansMono27 face font)
#define PWN_FACE_LOOK_R       "( \342\232\206_\342\232\206)"          // ( ⚆_⚆)
#define PWN_FACE_LOOK_L       "(\342\230\211_\342\230\211 )"          // (☉_☉ )
#define PWN_FACE_LOOK_R_HAPPY "( \342\227\225\342\200\277\342\227\225)"    // ( ◕‿◕)
#define PWN_FACE_LOOK_L_HAPPY "(\342\227\225\342\200\277\342\227\225 )"    // (◕‿◕ )
#define PWN_FACE_SLEEP        "(\342\207\200\342\200\277\342\200\277\342\206\274)" // (⇀‿‿↼)
#define PWN_FACE_SLEEP2       "(\342\211\226\342\200\277\342\200\277\342\211\226)" // (≖‿‿≖)
#define PWN_FACE_NAP          "(-o-)"
#define PWN_FACE_AWAKE        "(\342\227\225\342\200\277\342\200\277\342\227\225)" // (◕‿‿◕)
#define PWN_FACE_BORED        "(-__-)"
#define PWN_FACE_INTENSE      "(\302\260\342\226\203\342\226\203\302\260)"   // (°▃▃°)
#define PWN_FACE_COOL         "(\342\214\220\342\226\240_\342\226\240)"    // (⌐■_■)
#define PWN_FACE_HAPPY        "(\342\200\242\342\200\277\342\200\277\342\200\242)" // (•‿‿•)
#define PWN_FACE_GRATEFUL     "(^\342\200\277\342\200\277^)"          // (^‿‿^)
#define PWN_FACE_EXCITED      "(\341\265\224\342\227\241\342\227\241\341\265\224)" // (ᵔ◡◡ᵔ)
#define PWN_FACE_MOTIVATED    "(\342\230\274\342\200\277\342\200\277\342\230\274)" // (☼‿‿☼)
#define PWN_FACE_DEMOTIVATED  "(\342\211\226__\342\211\226)"          // (≖__≖)
#define PWN_FACE_SMART        "(\342\234\234\342\200\277\342\200\277\342\234\234)" // (✜‿‿✜)
#define PWN_FACE_LONELY       "(\330\250__\330\250)"                 // (ب__ب)
#define PWN_FACE_SAD          "(\342\225\245\342\230\201\342\225\245 )"       // (╥☁╥ )
#define PWN_FACE_ANGRY        "(-_-')"
#define PWN_FACE_FRIEND       "(\342\231\245\342\200\277\342\200\277\342\231\245)" // (♥‿‿♥)
#define PWN_FACE_BROKEN       "(\342\230\223\342\200\277\342\200\277\342\230\223)" // (☓‿‿☓)
#define PWN_FACE_DEBUG        "(#__#)"
#define PWN_FACE_UPLOAD       "(1__0)"
#define PWN_FACE_UPLOAD1      "(1__1)"
#define PWN_FACE_UPLOAD2      "(0__1)"

typedef struct {
    char channel[PWN_STR_LEN];
    char aps[PWN_STR_LEN];
    char uptime[PWN_STR_LEN];
    char face[PWN_FACE_LEN];
    char name[PWN_NAME_LEN];
    char status[PWN_STATUS_LEN];
    char shakes[PWN_STR_LEN];
    char mode[PWN_STR_LEN];
    char friend_face[PWN_FACE_LEN];
    char friend_name[PWN_FRIEND_NAME_LEN];
    int friend_rssi;
} pwn_ui_state_t;

void pwn_ui_init(void);
void pwn_ui_set_channel(const char *val);
void pwn_ui_set_aps(const char *val);
void pwn_ui_set_uptime(const char *val);
void pwn_ui_set_face(const char *val);
void pwn_ui_set_name(const char *val);
void pwn_ui_set_status(const char *val);
void pwn_ui_set_shakes(const char *val);
void pwn_ui_set_mode(const char *val);
void pwn_ui_set_friend(const char *face, const char *name, int rssi);
const char *pwn_ui_get_face(void);
const char *pwn_ui_get_channel(void);
const char *pwn_ui_get_aps(void);
const char *pwn_ui_get_uptime(void);
const char *pwn_ui_get_name(void);
const char *pwn_ui_get_status(void);
const char *pwn_ui_get_shakes(void);
const char *pwn_ui_get_mode(void);

void pwn_ui_commit(void);
void pwn_ui_full_commit(void);
void pwn_ui_force_update(void);

// Periodic UI maintenance (sleep-face animation). Cheap; call from the app's
// regular redraw tick (updateUi).
void pwn_ui_tick(void);

// Event helpers — set face+status and update
void pwn_ui_on_handshake(void);
void pwn_ui_on_deauth(const char *sta);
void pwn_ui_on_normal(void);
void pwn_ui_on_bored(void);
void pwn_ui_on_sad(void);
void pwn_ui_on_lonely(void);
void pwn_ui_on_excited(void);
void pwn_ui_on_motivated(void);
void pwn_ui_on_starting(void);

// Subscribe the UI's reactions to the firmware event bus (call after pwn_ui_init).
void pwn_ui_bind_events(void);

#ifdef __cplusplus
}
#endif
