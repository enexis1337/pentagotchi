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
#define PWN_STR_LEN        24
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
#define PWN_X_STATUS     110
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
#define PWN_LINE2_Y      108

// Pentagotchi-style ASCII faces (all plain ASCII, renderable by ANY u8g2 font)
#define PWN_FACE_LOOK_R       "( o_o)"
#define PWN_FACE_LOOK_L       "(O_O )"
#define PWN_FACE_LOOK_R_HAPPY "( ^_^)"
#define PWN_FACE_LOOK_L_HAPPY "(^_^ )"
#define PWN_FACE_SLEEP        "(-_-)"
#define PWN_FACE_SLEEP2       "(= =)"
#define PWN_FACE_AWAKE        "(^_^)"
#define PWN_FACE_BORED        "(-_-)"
#define PWN_FACE_INTENSE      "(>_<)"
#define PWN_FACE_COOL         "(0_0)"
#define PWN_FACE_HAPPY        "(*_*)"
#define PWN_FACE_GRATEFUL     "(^-^)"
#define PWN_FACE_EXCITED      "(^o^)"
#define PWN_FACE_MOTIVATED    "(^-^)"
#define PWN_FACE_DEMOTIVATED  "(=_=)"
#define PWN_FACE_SMART        "(*_*)"
#define PWN_FACE_LONELY       "(;_ ;)"
#define PWN_FACE_SAD          "(T_T)"
#define PWN_FACE_ANGRY        "(-_-')"
#define PWN_FACE_FRIEND       "(^_^)"
#define PWN_FACE_BROKEN       "(X_X)"
#define PWN_FACE_DEBUG        "(#_#)"
#define PWN_FACE_UPLOAD       "(._.)"
#define PWN_FACE_UPLOAD1      "(._.)"
#define PWN_FACE_UPLOAD2      "(._.)"

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
void pwn_ui_set_friend(const char *face, const char *name);

void pwn_ui_commit(void);
void pwn_ui_full_commit(void);
void pwn_ui_force_update(void);

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

#ifdef __cplusplus
}
#endif
