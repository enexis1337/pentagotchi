#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#include "u8g2.h"

#define EINK_WIDTH  250
#define EINK_HEIGHT 122

#define U8G2_TILE_W ((EINK_WIDTH + 7) / 8)
#define U8G2_TILE_H ((EINK_HEIGHT + 7) / 8)
#define U8G2_BUF_SIZE (U8G2_TILE_W * U8G2_TILE_H * 8)

extern u8g2_t g_u8g2;
extern uint8_t g_u8g2_buf[];

typedef struct {
    bool sd_initialized;
    uint32_t sd_size_mb;
    bool display_initialized;
    char device_name[32];
} eink_status_t;

esp_err_t eink_init(void);
esp_err_t eink_clear(void);
esp_err_t eink_show_boot_status(const eink_status_t *status);
esp_err_t eink_print_text(const char *text, uint16_t x, uint16_t y);
esp_err_t eink_init_partial(void);
esp_err_t eink_refresh(void);  // Partial refresh (быстро, без мерцания)
esp_err_t eink_full_refresh(void);  // Full refresh (медленно, с мерцанием)
esp_err_t eink_set_rotation(int rotation);
void eink_deinit(void);

// Управление режимом обновления
void eink_set_full_refresh_interval(uint32_t seconds);  // Установить интервал full refresh (по умолчанию 600 сек = 10 мин)
bool eink_should_do_full_refresh(void);  // Проверить, нужен ли full refresh
void eink_mark_full_refresh_done(void);  // Отметить что full refresh выполнен
