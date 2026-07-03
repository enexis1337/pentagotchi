/**
 * eink_regions.h - Dirty regions tracking for E-ink display
 * Позволяет обновлять только изменившиеся области экрана
 */

#ifndef EINK_REGIONS_H
#define EINK_REGIONS_H

#include <stdint.h>
#include <stdbool.h>

// Предопределенные регионы экрана
typedef enum {
    REGION_CHANNEL = 0,   // CH XX (верхний левый)
    REGION_APS,           // APS X (верхний левый)
    REGION_UPTIME,        // UP XX:XX:XX (верхний правый)
    REGION_NAME,          // pentagotchi> (под линией)
    REGION_FACE,          // (◕‿‿◕) (центр)
    REGION_STATUS,        // Текст статуса (под лицом)
    REGION_FRIEND,        // Friend info (если есть)
    REGION_SHAKES,        // PWND X (X) (нижний левый)
    REGION_MODE,          // AUTO/AI (нижний правый)
    REGION_MAX
} eink_region_t;

// Координаты регионов (из pwn_ui.h)
typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
} region_rect_t;

// Инициализация системы регионов
void eink_regions_init(void);

// Отметить регион как грязный (требует обновления)
void eink_region_mark_dirty(eink_region_t region);

// Отметить все регионы как грязные
void eink_regions_mark_all_dirty(void);

// Проверить есть ли грязные регионы
bool eink_regions_has_dirty(void);

// Обновить только грязные регионы
void eink_regions_refresh_dirty(void);

// Очистить флаги грязных регионов
void eink_regions_clear_dirty(void);

// Получить координаты региона
const region_rect_t* eink_region_get_rect(eink_region_t region);

#endif // EINK_REGIONS_H
