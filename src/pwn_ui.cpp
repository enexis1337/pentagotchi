#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "eink_display.h"
#include "eink_regions.h"
#include "pwn_ui.h"

static const char *TAG = "pwn_ui";

static pwn_ui_state_t s_state;
static pwn_ui_state_t s_prev;
static bool s_dirty = false;
static bool s_force = false;

static bool str_changed(const char *a, const char *b)
{
    return strcmp(a, b) != 0;
}

static void mark_dirty(void)
{
    s_dirty = true;
}

static void check_dirty(void)
{
    if (str_changed(s_state.channel, s_prev.channel)) { s_dirty = true; strcpy(s_prev.channel, s_state.channel); }
    if (str_changed(s_state.aps, s_prev.aps)) { s_dirty = true; strcpy(s_prev.aps, s_state.aps); }
    if (str_changed(s_state.uptime, s_prev.uptime)) { s_dirty = true; strcpy(s_prev.uptime, s_state.uptime); }
    if (str_changed(s_state.face, s_prev.face)) { s_dirty = true; strcpy(s_prev.face, s_state.face); }
    if (str_changed(s_state.name, s_prev.name)) { s_dirty = true; strcpy(s_prev.name, s_state.name); }
    if (str_changed(s_state.status, s_prev.status)) { s_dirty = true; strcpy(s_prev.status, s_state.status); }
    if (str_changed(s_state.shakes, s_prev.shakes)) { s_dirty = true; strcpy(s_prev.shakes, s_state.shakes); }
    if (str_changed(s_state.mode, s_prev.mode)) { s_dirty = true; strcpy(s_prev.mode, s_state.mode); }
    if (str_changed(s_state.friend_face, s_prev.friend_face)) { s_dirty = true; strcpy(s_prev.friend_face, s_state.friend_face); }
    if (str_changed(s_state.friend_name, s_prev.friend_name)) { s_dirty = true; strcpy(s_prev.friend_name, s_state.friend_name); }
}

static void draw_labeled_value(int x, int y, const char *label, const char *value,
                                const uint8_t *label_font, const uint8_t *value_font)
{
    if (label) {
        u8g2_SetFont(&g_u8g2, label_font);
        u8g2_SetFontPosTop(&g_u8g2);
        u8g2_DrawStr(&g_u8g2, x, y, label);

        int label_w = u8g2_GetStrWidth(&g_u8g2, label);
        u8g2_SetFont(&g_u8g2, value_font);
        u8g2_SetFontPosTop(&g_u8g2);
        u8g2_DrawStr(&g_u8g2, x + label_w + 5, y, value);
    } else {
        u8g2_SetFont(&g_u8g2, value_font);
        u8g2_SetFontPosTop(&g_u8g2);
        u8g2_DrawStr(&g_u8g2, x, y, value);
    }
}

static void draw_text(int x, int y, const char *text, const uint8_t *font)
{
    u8g2_SetFont(&g_u8g2, font);
    u8g2_SetFontPosTop(&g_u8g2);
    u8g2_DrawStr(&g_u8g2, x, y, text);
}

// True if the token at p is a full MAC address "AA:BB:CC:DD:EE:FF" (17 chars)
static bool is_mac_token(const char *p)
{
    if (!p) return false;
    for (int g = 0; g < 6; g++) {
        for (int i = 0; i < 2; i++) {
            char c = p[g * 3 + i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                return false;
            }
        }
        if (g < 5 && p[g * 3 + 2] != ':') return false;
    }
    char n = p[17];
    return n == '\0' || n == ' ' || n == '\n';
}

static void pwn_ui_render(void)
{
    memset(g_u8g2_buf, 0xFF, U8G2_BUF_SIZE);

    u8g2_SetFont(&g_u8g2, u8g2_font_7x13_tf);
    const int oneCharW = u8g2_GetStrWidth(&g_u8g2, "0");

    draw_labeled_value(PWN_X_CH, PWN_Y_CH, "CH", s_state.channel,
                       u8g2_font_6x12_tf, u8g2_font_7x13_tf);
    draw_labeled_value(PWN_X_APS, PWN_Y_APS, "APS", s_state.aps,
                       u8g2_font_6x12_tf, u8g2_font_7x13_tf);
    draw_labeled_value(PWN_X_UPTIME + oneCharW, PWN_Y_UPTIME, "UP", s_state.uptime,
                       u8g2_font_6x12_tf, u8g2_font_7x13_tf);

    u8g2_DrawHLine(&g_u8g2, 0, PWN_LINE1_Y, PWN_UI_W);

    // Name + prompt symbol, one character gap between them
    u8g2_SetFont(&g_u8g2, u8g2_font_7x13_tf);
    u8g2_SetFontPosTop(&g_u8g2);
    u8g2_DrawStr(&g_u8g2, PWN_X_NAME, PWN_Y_NAME, s_state.name);
    int nameW = u8g2_GetStrWidth(&g_u8g2, s_state.name);
    int spaceW = u8g2_GetStrWidth(&g_u8g2, " ");
    u8g2_DrawStr(&g_u8g2, PWN_X_NAME + nameW + spaceW, PWN_Y_NAME, PWN_NAME_PROMPT);

    draw_text(PWN_X_FACE, PWN_Y_FACE, s_state.face, u8g2_font_courB24_tf);

    u8g2_SetFont(&g_u8g2, u8g2_font_6x12_tf);
    u8g2_SetFontPosTop(&g_u8g2);
    int line_h = 12;
    const int kMaxChars = 22;
    const char *p = s_state.status;
    char lines[8][64];
    int n = 0;

    while (*p && n < (int)(sizeof(lines) / sizeof(lines[0]))) {
        while (*p == ' ') p++;
        if (*p == '\n') { p++; continue; }
        if (!*p) break;

        int i = 0;
        if (is_mac_token(p)) {
            // MAC address always on its own (new) line, never split
            for (int k = 0; k < 17 && p[k]; k++) lines[n][i++] = p[k];
            p += 17;
        } else {
            int j = 0;
            // Skip leading spaces of the whole phrase (never between tokens)
            while (p[j] == ' ') j++;
            while (p[j] && p[j] != '\n' && i < kMaxChars) {
                if (i > 0 && is_mac_token(p + j)) break; // MAC moves to a new line
                int wl = 0;
                while (p[j + wl] && p[j + wl] != ' ' && p[j + wl] != '\n') wl++;
                int need = i + wl + (i > 0 ? 1 : 0);
                if (need > kMaxChars) break; // word doesn't fit, wrap
                if (i > 0) lines[n][i++] = ' ';
                for (int k = 0; k < wl; k++) lines[n][i++] = p[j + k];
                j += wl;
                while (p[j] == ' ') j++; // collapse multiple separators
            }
            // hard cut at kMaxChars even for long words
            while (p[j] && p[j] != '\n' && i < kMaxChars) {
                if (is_mac_token(p + j)) break;
                lines[n][i++] = p[j];
                j++;
            }
            p += j;
        }
        if (*p == '\n') p++;
        lines[n][i] = '\0';
        n++;
    }

    // Если фраза длиннее колонки — показываем ХВОСТ, чтобы последние
    // символы (часто MAC) остались на экране
    int maxLines = (PWN_LINE2_Y - PWN_Y_STATUS) / line_h;
    int start = (n > maxLines) ? (n - maxLines) : 0;
    int sy = PWN_Y_STATUS;
    for (int l = start; l < n; l++) {
        u8g2_DrawStr(&g_u8g2, PWN_X_STATUS, sy, lines[l]);
        sy += line_h;
    }

    if (strlen(s_state.friend_face) > 0) {
        draw_text(PWN_X_FRIEND_FACE, PWN_Y_FRIEND_FACE, s_state.friend_face, u8g2_font_6x10_tf);
    }
    if (strlen(s_state.friend_name) > 0) {
        draw_text(PWN_X_FRIEND_NAME, PWN_Y_FRIEND_NAME, s_state.friend_name, u8g2_font_6x10_tf);
    }

    u8g2_DrawHLine(&g_u8g2, 0, PWN_LINE2_Y, PWN_UI_W);

    draw_labeled_value(PWN_X_SHAKES, PWN_Y_SHAKES, "PWND", s_state.shakes,
                       u8g2_font_6x12_tf, u8g2_font_7x13_tf);
    draw_text(PWN_X_MODE, PWN_Y_MODE, s_state.mode, u8g2_font_7x13_tf);
}

void pwn_ui_set_channel(const char *val) { 
    strncpy(s_state.channel, val, sizeof(s_state.channel) - 1); 
    mark_dirty(); 
    eink_region_mark_dirty(REGION_CHANNEL);
}

void pwn_ui_set_aps(const char *val) { 
    strncpy(s_state.aps, val, sizeof(s_state.aps) - 1); 
    mark_dirty(); 
    eink_region_mark_dirty(REGION_APS);
}

void pwn_ui_set_uptime(const char *val) { 
    strncpy(s_state.uptime, val, sizeof(s_state.uptime) - 1); 
    mark_dirty(); 
    eink_region_mark_dirty(REGION_UPTIME);
}

void pwn_ui_set_face(const char *val) { 
    strncpy(s_state.face, val, sizeof(s_state.face) - 1); 
    mark_dirty(); 
    eink_region_mark_dirty(REGION_FACE);
}

void pwn_ui_set_name(const char *val) { 
    strncpy(s_state.name, val, sizeof(s_state.name) - 1); 
    mark_dirty(); 
    eink_region_mark_dirty(REGION_NAME);
}

void pwn_ui_set_status(const char *val) { 
    strncpy(s_state.status, val, sizeof(s_state.status) - 1); 
    mark_dirty(); 
    eink_region_mark_dirty(REGION_STATUS);
}

void pwn_ui_set_shakes(const char *val) { 
    strncpy(s_state.shakes, val, sizeof(s_state.shakes) - 1); 
    mark_dirty(); 
    eink_region_mark_dirty(REGION_SHAKES);
}

void pwn_ui_set_mode(const char *val) { 
    strncpy(s_state.mode, val, sizeof(s_state.mode) - 1); 
    mark_dirty(); 
    eink_region_mark_dirty(REGION_MODE);
}

void pwn_ui_set_friend(const char *face, const char *name)
{
    if (face) strncpy(s_state.friend_face, face, sizeof(s_state.friend_face) - 1);
    else s_state.friend_face[0] = '\0';
    if (name) strncpy(s_state.friend_name, name, sizeof(s_state.friend_name) - 1);
    else s_state.friend_name[0] = '\0';
    mark_dirty();
    eink_region_mark_dirty(REGION_FRIEND);
}

void pwn_ui_commit(void)
{
    check_dirty();
    if (s_dirty || s_force) {
        pwn_ui_render();
        
        // Используем систему dirty regions для умного обновления
        if (eink_regions_has_dirty()) {
            eink_regions_refresh_dirty();
        }
        
        s_dirty = false;
        s_force = false;
    }
}

void pwn_ui_full_commit(void)
{
    pwn_ui_render();
    eink_full_refresh();
}

void pwn_ui_force_update(void)
{
    s_force = true;
    pwn_ui_commit();
}

void pwn_ui_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    memset(&s_prev, 0, sizeof(s_prev));

    strcpy(s_state.channel, "00");
    strcpy(s_state.aps, "0 (00)");
    strcpy(s_state.uptime, "00:00:00");
    strcpy(s_state.face, PWN_FACE_SLEEP);
    strcpy(s_state.name, "pentagotchi");
    strcpy(s_state.status, "starting...");
    strcpy(s_state.shakes, "0 (00)");
    strcpy(s_state.mode, "AUTO");

    s_force = true;
    s_dirty = false;

    // Инициализировать систему dirty regions
    eink_regions_init();
    eink_regions_mark_all_dirty();  // Первый раз - все грязные

    ESP_LOGI(TAG, "UI initialized (%dx%d)", PWN_UI_W, PWN_UI_H);
}

void pwn_ui_on_handshake(void)
{
    pwn_ui_set_face(PWN_FACE_HAPPY);
    pwn_ui_set_status("Got a handshake!");
    pwn_ui_force_update();
}

void pwn_ui_on_deauth(const char *sta)
{
    pwn_ui_set_face(PWN_FACE_COOL);
    char buf[PWN_STATUS_LEN];
    snprintf(buf, sizeof(buf), "Deauth sent to %s", sta);
    pwn_ui_set_status(buf);
    pwn_ui_force_update();
}

void pwn_ui_on_normal(void)
{
    pwn_ui_set_face(PWN_FACE_AWAKE);
    pwn_ui_set_status("Hello! I'm pentagotchi");
    pwn_ui_force_update();
}

void pwn_ui_on_bored(void)
{
    pwn_ui_set_face(PWN_FACE_BORED);
    pwn_ui_set_status("Bored...");
    pwn_ui_force_update();
}

void pwn_ui_on_sad(void)
{
    pwn_ui_set_face(PWN_FACE_SAD);
    pwn_ui_set_status("Sad...");
    pwn_ui_force_update();
}

void pwn_ui_on_lonely(void)
{
    pwn_ui_set_face(PWN_FACE_LONELY);
    pwn_ui_set_status("Feeling lonely...");
    pwn_ui_force_update();
}

void pwn_ui_on_excited(void)
{
    pwn_ui_set_face(PWN_FACE_EXCITED);
    pwn_ui_set_status("Excited!");
    pwn_ui_force_update();
}

void pwn_ui_on_motivated(void)
{
    pwn_ui_set_face(PWN_FACE_MOTIVATED);
    pwn_ui_set_status("Motivated!");
    pwn_ui_force_update();
}

void pwn_ui_on_starting(void)
{
    pwn_ui_set_face(PWN_FACE_AWAKE);
    pwn_ui_set_status("Starting up...");
    pwn_ui_force_update();
}
