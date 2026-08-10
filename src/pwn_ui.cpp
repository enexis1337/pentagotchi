#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_random.h"
#include "eink_display.h"
#include "eink_regions.h"
#include "pwn_ui.h"
#include "pentagotchi_events.h"

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
    if (s_state.friend_rssi != s_prev.friend_rssi) { s_dirty = true; s_prev.friend_rssi = s_state.friend_rssi; }
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
static bool is_mac_byte(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool is_mac_punct(char c)
{
    return c == ',' || c == '.' || c == '!' || c == '?' || c == ':' || c == ';' ||
           c == '\'' || c == '"' || c == ')';
}

static bool is_mac_token(const char *p)
{
    if (!p) return false;
    for (int g = 0; g < 6; g++) {
        for (int i = 0; i < 2; i++) {
            if (!is_mac_byte(p[g * 3 + i])) return false;
        }
        if (g < 5 && p[g * 3 + 2] != ':') return false;
    }
    char n = p[17];
    return n == '\0' || n == ' ' || n == '\n' || is_mac_punct(n);
}

static void pwn_ui_render(void)
{
    memset(g_u8g2_buf, 0xFF, U8G2_BUF_SIZE);

    u8g2_SetFont(&g_u8g2, u8g2_font_7x13_tf);
    const int oneCharW = u8g2_GetStrWidth(&g_u8g2, "0");

    draw_labeled_value(PWN_X_CH, PWN_Y_CH, "CH", s_state.channel,
                       u8g2_font_6x13B_tf, u8g2_font_7x13_tf);
    draw_labeled_value(PWN_X_APS, PWN_Y_APS, "APS", s_state.aps,
                       u8g2_font_6x13B_tf, u8g2_font_7x13_tf);
    draw_labeled_value(PWN_X_UPTIME + oneCharW, PWN_Y_UPTIME, "UP", s_state.uptime,
                       u8g2_font_6x13B_tf, u8g2_font_7x13_tf);

    u8g2_DrawHLine(&g_u8g2, 0, PWN_LINE1_Y, PWN_UI_W);

    // Name + prompt symbol, one character gap between them
    u8g2_SetFont(&g_u8g2, u8g2_font_7x13B_tf);
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
            // MAC (plus trailing punctuation) always on its own line, never split
            for (int k = 0; k < 17 && p[k]; k++) lines[n][i++] = p[k];
            int k = 17;
            while (p[k] && is_mac_punct(p[k])) lines[n][i++] = p[k++];
            p += k;
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

    // Friend: face + signal bars + "name N (M)"
    if (strlen(s_state.friend_face) > 0 || s_state.friend_rssi > -1000) {
        int fx = PWN_X_FRIEND_FACE;
        if (strlen(s_state.friend_face) > 0) {
            draw_text(fx, PWN_Y_FRIEND_FACE, s_state.friend_face, u8g2_font_6x10_tf);
            fx += u8g2_GetStrWidth(&g_u8g2, s_state.friend_face) + 4;
        }

        int bars = 0;
        if (s_state.friend_rssi >= -55) bars = 4;
        else if (s_state.friend_rssi >= -65) bars = 3;
        else if (s_state.friend_rssi >= -75) bars = 2;
        else if (s_state.friend_rssi >= -85) bars = 1;

        if (bars > 0) {
            for (int b = 0; b < 4; b++) {
                int h = 4 + b * 3;
                int yTop = PWN_Y_FRIEND_FACE + 12 - h;
                if (b < bars) {
                    u8g2_DrawBox(&g_u8g2, fx, yTop, 4, h);
                } else {
                    u8g2_DrawFrame(&g_u8g2, fx, yTop, 4, h);
                }
                fx += 7;
            }
            fx += 3;
        }

        if (strlen(s_state.friend_name) > 0) {
            draw_text(fx, PWN_Y_FRIEND_FACE, s_state.friend_name, u8g2_font_6x10_tf);
        }
    }

    u8g2_DrawHLine(&g_u8g2, 0, PWN_LINE2_Y, PWN_UI_W);

    draw_labeled_value(PWN_X_SHAKES, PWN_Y_SHAKES, "PWND", s_state.shakes,
                       u8g2_font_6x13B_tf, u8g2_font_7x13_tf);
    draw_text(PWN_X_MODE, PWN_Y_MODE, s_state.mode, u8g2_font_7x13B_tf);
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

void pwn_ui_set_friend(const char *face, const char *name, int rssi)
{
    if (face) strncpy(s_state.friend_face, face, sizeof(s_state.friend_face) - 1);
    else s_state.friend_face[0] = '\0';
    if (name) strncpy(s_state.friend_name, name, sizeof(s_state.friend_name) - 1);
    else s_state.friend_name[0] = '\0';
    s_state.friend_rssi = rssi;
    mark_dirty();
    eink_region_mark_dirty(REGION_FRIEND);
}

const char *pwn_ui_get_face(void)
{
    return s_state.face;
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
    s_state.friend_rssi = -1000;

    s_force = true;
    s_dirty = false;

    // Инициализировать систему dirty regions
    eink_regions_init();
    eink_regions_mark_all_dirty();  // Первый раз - все грязные

    ESP_LOGI(TAG, "UI initialized (%dx%d)", PWN_UI_W, PWN_UI_H);
}

static const char *const kHandshakePhrases[] = {
    "Yay! I got a\nnew present!",
    "High five!\nGot the handshake!",
    "Aww, someone shared\na secret with me!",
    "Got it!\nWe're officially\nbest friends!",
    "Yay, another souvenir\nfor my collection!",
    "Heart captured! <3",
    "Mmm, fresh packets!",
    "Oops, someone dropped\ntheir keys!",
    "Yoink! Mine now!",
    "That was almost\ntoo easy.",
    "Thanks for the keys\nto the kingdom!",
    "Handshake stored in\nmemory!",
};

void pwn_ui_on_handshake(void)
{
    pwn_ui_set_face(PWN_FACE_HAPPY);
    const size_t n = sizeof(kHandshakePhrases) / sizeof(kHandshakePhrases[0]);
    pwn_ui_set_status(kHandshakePhrases[esp_random() % n]);
    pwn_ui_force_update();
}

static const char *const kDeauthPhrases[] = {
    "Hey %s,\nlet's be friends!",
    "%s, you look so\nsoft! Group hug?",
    "Hi %s! I made us\nfriendship bracelets!",
    "%s, do you want\nto share a handshake?",
    "Don't be shy,\n%s!",
    "Yay, %s!\nNew friends are\nthe best!",
    "%s, your signal\nis glowing today!",
    "%s, stop running,\nI just want a bite!",
    "Smells like fresh\npackets from %s...",
    "Nom nom nom...\nThanks for the\nhandshake, %s!",
    "Hand over the\nhandshake, %s,\nand nobody gets hurt!",
    "%s, prepare to\nget deauthed.",
};

void pwn_ui_on_deauth(const char *sta)
{
    pwn_ui_set_face(PWN_FACE_COOL);
    const size_t n = sizeof(kDeauthPhrases) / sizeof(kDeauthPhrases[0]);
    const char *fmt = kDeauthPhrases[esp_random() % n];
    char buf[PWN_STATUS_LEN];
    snprintf(buf, sizeof(buf), fmt, sta ? sta : "???");
    pwn_ui_set_status(buf);
    pwn_ui_force_update();
}

void pwn_ui_on_normal(void)
{
    pwn_ui_set_face(PWN_FACE_AWAKE);
    pwn_ui_set_status("");
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

// ---- event bus subscribers (see pwn_ui_bind_events) ----
// Handlers that only set state (no immediate render) are safe to run from the
// promiscuous RX context; the actual refresh happens in the main loop.

static void ui_on_event_handshake(const pwn_event_t *ev)
{
    (void)ev;
    pwn_ui_on_handshake();
}

static void ui_on_event_deauth(const pwn_event_t *ev)
{
    pwn_ui_on_deauth(ev && ev->str ? ev->str : "");
}

static const char *const kPeerDetectedPhrases[] = {
    "Hi %s!\nWant to be friends?",
    "Aww, look!\nIt's %s!",
    "Yay! %s\nis here!",
    "Hello %s!\nLet's hunt together!",
    "High five, %s!\nSo happy to see you!",
    "Ooh, %s!\nYou look so cute!",
    "Hey %s, leave some\nhandshakes for me!",
    "Ooh, %s is here!\nShare your snacks!",
    "Are you gonna eat\nthat packet, %s?",
    "Two heads hunt better!\nRight, %s?",
    "Quick %s, let's\nsteal all the WiFi!",
    "Oh look, %s\nfinally showed up!",
    "Try to keep up\nwith me, %s!",
    "Look out, %s,\nthe pro is working!",
    "Nice face, %s!\nMine's better though.",
    "Hey %s, watching\nand learning?",
    "Back off %s,\nthis is my territory!",
    "This channel\nisn't big enough\nfor us, %s!",
    "Don't touch my\nhandshakes, %s!",
};

static void ui_on_event_peer_detected(const pwn_event_t *ev)
{
    pwn_ui_set_face(PWN_FACE_GRATEFUL);
    const size_t n = sizeof(kPeerDetectedPhrases) / sizeof(kPeerDetectedPhrases[0]);
    const char *name = (ev && ev->str && strlen(ev->str)) ? ev->str : "buddy";
    char buf[PWN_STATUS_LEN];
    snprintf(buf, sizeof(buf), kPeerDetectedPhrases[esp_random() % n], name);
    pwn_ui_set_status(buf);
}

static const char *const kFriendPhrases[] = {
    "We're officially\nbesties now, %s!",
    "Yay! %s and I\nare friends now!",
    "Friendship status\nwith %s:\nUNLOCKED!",
    "It's official!\n%s is my\nnew friend!",
    "Aww, %s is my\nfriend now! <3",
    "I made a new\nbest friend: %s!",
    "%s accepted\nmy friendship!",
    "%s is my friend\nnow! Time to\nshare snacks!",
    "Official wifi-stealing\npartners with %s!",
    "We are friends now,\n%s! Don't eat\nmy handshakes!",
    "Friendship formed!\nNow give me half\nyour packets, %s!",
    "%s is my friend!\nPartners in crime!",
    "%s just leveled\nup to my friend!",
    "Lucky you, %s,\nwe're officially\nfriends now.",
    "Congrats %s, you\nmade the friend list!",
    "Friendship complete!\nTry to match my\nvibe, %s.",
    "Welcome to the\ncool kids club,\n%s!",
};

static void ui_on_event_friend(const pwn_event_t *ev)
{
    const size_t n = sizeof(kFriendPhrases) / sizeof(kFriendPhrases[0]);
    const char *name = (ev && ev->str && strlen(ev->str)) ? ev->str : "buddy";
    char buf[PWN_STATUS_LEN];
    snprintf(buf, sizeof(buf), kFriendPhrases[esp_random() % n], name);
    pwn_ui_set_status(buf);
}

static const char *const kPeerGonePhrases[] = {
    "Bye bye, %s!\nCome back soon!",
    "Miss you already,\n%s!",
    "Stay safe out\nthere, %s!",
    "See you later,\nbestie %s!",
    "Don't forget about\nme, %s! <3",
    "Until next time,\n%s!",
    "Bye %s, more\nsnacks for me!",
    "Save some\nhandshakes for me,\n%s!",
    "Bye %s! I'll eat\nyour share of packets!",
    "See ya %s! Go\nraid another channel!",
    "%s left!\nTime to eat all\nthe data!",
    "Bye %s, try not\nto get pwned!",
    "Later, %s!\nDon't miss me\ntoo much.",
    "Bye %s! Back to\nbeing the star of\nthe show.",
    "Don't be a\nstranger, %s!",
    "%s couldn't handle\nmy speed!",
};

static void ui_on_event_peer_gone(const pwn_event_t *ev)
{
    const size_t n = sizeof(kPeerGonePhrases) / sizeof(kPeerGonePhrases[0]);
    const char *name = (ev && ev->str && strlen(ev->str)) ? ev->str : "buddy";
    char buf[PWN_STATUS_LEN];
    snprintf(buf, sizeof(buf), kPeerGonePhrases[esp_random() % n], name);
    pwn_ui_set_status(buf);
}

static void ui_on_event_stats_cleared(const pwn_event_t *ev)
{
    (void)ev;
    pwn_ui_set_status("Stats cleared");
    pwn_ui_force_update();
}

static void ui_on_event_boot(const pwn_event_t *ev)
{
    (void)ev;
    pwn_ui_set_face(PWN_FACE_SLEEP);
    pwn_ui_set_status("Waking up...");
    pwn_ui_full_commit();
    delay(300);

    pwn_ui_set_face(PWN_FACE_AWAKE);
    pwn_ui_set_status("Hello! I'm pentagotchi");
    pwn_ui_full_commit();
    delay(500);

    pwn_ui_on_normal();
    pwn_ui_full_commit();
}

static void ui_on_event_scan_cycle(const pwn_event_t *ev)
{
    char buf[PWN_STATUS_LEN];
    const int ch = ev ? (int)ev->value : 0;
    snprintf(buf, sizeof(buf), "Scanning ch %d", ch);
    pwn_ui_set_status(buf);
}

static void ui_on_event_channel(const pwn_event_t *ev)
{
    const int ch = ev ? (int)ev->value : 0;
    if (ch <= 1) {
        pwn_ui_set_face(PWN_FACE_LOOK_L);
    } else if (ch >= 11) {
        pwn_ui_set_face(PWN_FACE_LOOK_R);
    } else {
        pwn_ui_set_face(PWN_FACE_AWAKE);
    }
    char buf[PWN_STATUS_LEN];
    snprintf(buf, sizeof(buf), "Hopping to ch %d", ch);
    pwn_ui_set_status(buf);
}

static void ui_on_event_ap_detected(const pwn_event_t *ev)
{
    char buf[PWN_STATUS_LEN];
    if (ev && ev->value > 0) {
        snprintf(buf, sizeof(buf), "Ooh, I see an AP on ch %d!", (int)ev->value);
    } else {
        snprintf(buf, sizeof(buf), "Ooh, I see an AP!");
    }
    pwn_ui_set_status(buf);
}

static const char *const kPeerEncounterPhrases[] = {
    "Yay, %s!\nYou're back!",
    "I missed you,\n%s!",
    "Bestie!\n%s returned!",
    "Look who's back!\nHi %s!",
    "%s!\nHugs incoming!",
    "My favorite friend\n%s is back!",
    "So good to see you\nagain, %s!",
    "Back for more\nsnacks, %s?",
    "Hey %s, did you\nbring me handshakes?",
    "Together again!\nLet's raid, %s!",
    "Did you miss my\npacket stealing, %s?",
    "Look, it's my\npartner in crime,\n%s!",
    "Oh, you again,\n%s?",
};

static void ui_on_event_peer_encounter(const pwn_event_t *ev)
{
    const size_t n = sizeof(kPeerEncounterPhrases) / sizeof(kPeerEncounterPhrases[0]);
    const char *name = (ev && ev->str && strlen(ev->str)) ? ev->str : "buddy";
    char buf[PWN_STATUS_LEN];
    snprintf(buf, sizeof(buf), kPeerEncounterPhrases[esp_random() % n], name);
    pwn_ui_set_status(buf);
}

void pwn_ui_bind_events(void)
{
    pwn_events_subscribe(PWN_EVENT_BOOT, ui_on_event_boot, "ui_boot");
    pwn_events_subscribe(PWN_EVENT_SCAN_CYCLE, ui_on_event_scan_cycle, "ui_scan");
    pwn_events_subscribe(PWN_EVENT_CHANNEL_CHANGED, ui_on_event_channel, "ui_channel");
    pwn_events_subscribe(PWN_EVENT_AP_DETECTED, ui_on_event_ap_detected, "ui_ap");
    pwn_events_subscribe(PWN_EVENT_HANDSHAKE, ui_on_event_handshake, "ui_handshake");
    pwn_events_subscribe(PWN_EVENT_DEAUTH_SENT, ui_on_event_deauth, "ui_deauth");
    pwn_events_subscribe(PWN_EVENT_PEER_DETECTED, ui_on_event_peer_detected, "ui_peer");
    pwn_events_subscribe(PWN_EVENT_PEER_ENCOUNTER, ui_on_event_peer_encounter, "ui_encounter");
    pwn_events_subscribe(PWN_EVENT_FRIEND, ui_on_event_friend, "ui_friend");
    pwn_events_subscribe(PWN_EVENT_PEER_GONE, ui_on_event_peer_gone, "ui_peer_gone");
    pwn_events_subscribe(PWN_EVENT_STATS_CLEARED, ui_on_event_stats_cleared, "ui_stats");
}
