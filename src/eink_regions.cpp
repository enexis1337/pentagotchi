/**
 * eink_regions.cpp - Dirty regions implementation
 */

#include "eink_regions.h"
#include "eink_display.h"
#include "pwn_ui.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "eink_regions";

// Флаги грязных регионов
static bool s_dirty[REGION_MAX] = {false};

// Координаты регионов (based on pwn_ui.h positions)
static const region_rect_t s_regions[REGION_MAX] = {
    // REGION_CHANNEL - "CH XX"
    {PWN_X_CH, PWN_Y_CH, 50, 15},
    
    // REGION_APS - "APS X (XX)"
    {PWN_X_APS, PWN_Y_APS, 80, 15},
    
    // REGION_UPTIME - "UP XX:XX:XX"
    {PWN_X_UPTIME, PWN_Y_UPTIME, 100, 15},

    // REGION_NAME - "pentagotchi>"
    {PWN_X_NAME, PWN_Y_NAME, 150, 15},
    
    // REGION_FACE - "(◕‿‿◕)"
    {PWN_X_FACE, PWN_Y_FACE, 100, 30},
    
    // REGION_STATUS - Status text (multiline possible)
    {PWN_X_STATUS, PWN_Y_STATUS, 230, 60},
    
    // REGION_FRIEND - Friend face + name
    {PWN_X_FRIEND_FACE, PWN_Y_FRIEND_FACE, 150, 20},
    
    // REGION_SHAKES - "PWND X (XX)"
    {PWN_X_SHAKES, PWN_Y_SHAKES, 120, 15},
    
    // REGION_MODE - "AUTO" / "AI"
    {PWN_X_MODE, PWN_Y_MODE, 50, 15}
};

void eink_regions_init(void)
{
    memset(s_dirty, 0, sizeof(s_dirty));
    ESP_LOGI(TAG, "Dirty regions system initialized");
}

void eink_region_mark_dirty(eink_region_t region)
{
    if (region >= REGION_MAX) return;
    s_dirty[region] = true;
    ESP_LOGD(TAG, "Region %d marked dirty", region);
}

void eink_regions_mark_all_dirty(void)
{
    for (int i = 0; i < REGION_MAX; i++) {
        s_dirty[i] = true;
    }
    ESP_LOGD(TAG, "All regions marked dirty");
}

bool eink_regions_has_dirty(void)
{
    for (int i = 0; i < REGION_MAX; i++) {
        if (s_dirty[i]) return true;
    }
    return false;
}

void eink_regions_refresh_dirty(void)
{
    if (!eink_regions_has_dirty()) {
        ESP_LOGD(TAG, "No dirty regions, skipping refresh");
        return;
    }
    
    // Подсчитываем количество грязных регионов
    int dirty_count = 0;
    for (int i = 0; i < REGION_MAX; i++) {
        if (s_dirty[i]) dirty_count++;
    }
    
    ESP_LOGD(TAG, "Refreshing %d dirty region(s)", dirty_count);
    
    // Если грязных регионов больше половины - проще обновить весь экран
    if (dirty_count > REGION_MAX / 2) {
        ESP_LOGD(TAG, "Too many dirty regions (%d/%d), doing full refresh", 
                 dirty_count, REGION_MAX);
        eink_refresh();
        eink_regions_clear_dirty();
        return;
    }
    
    // ВАЖНО: Для SSD1680 "настоящий" partial refresh по регионам не поддерживается
    // Поэтому мы всё равно обновляем весь экран, но используем WAS_PARTIAL_REFRESH
    // Dirty regions помогают только решить КОГДА обновлять, а не КАК
    
    // TODO: Если библиотека поддерживает настоящий partial по координатам - реализовать здесь
    // Например: ssd1680_refresh_region(s_disp, x, y, w, h, WAS_PARTIAL_REFRESH);
    
    // Пока просто делаем partial всего экрана
    eink_refresh();
    eink_regions_clear_dirty();
}

void eink_regions_clear_dirty(void)
{
    memset(s_dirty, 0, sizeof(s_dirty));
}

const region_rect_t* eink_region_get_rect(eink_region_t region)
{
    if (region >= REGION_MAX) return NULL;
    return &s_regions[region];
}
