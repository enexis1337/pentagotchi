#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#include <Arduino.h>
#include <SPI.h>
#include <U8g2lib.h>

#define EINK_WIDTH  250
#define EINK_HEIGHT 122

#define U8G2_TILE_W ((EINK_WIDTH + 7) / 8)
#define U8G2_TILE_H ((EINK_HEIGHT + 7) / 8)
#define U8G2_BUF_SIZE (U8G2_TILE_W * U8G2_TILE_H * 8)

extern u8g2_t g_u8g2;
extern uint8_t g_u8g2_buf[];

esp_err_t eink_init(void);
esp_err_t eink_refresh(void);  // Partial refresh (быстро, без мерцания)
esp_err_t eink_full_refresh(void);  // Full refresh (медленно, с мерцанием)
esp_err_t eink_set_rotation(int rotation);
void eink_deinit(void);

// Управление режимом обновления
void eink_set_full_refresh_interval(uint32_t seconds);  // Установить интервал full refresh (по умолчанию 600 сек = 10 мин)
bool eink_should_do_full_refresh(void);  // Проверить, нужен ли full refresh
void eink_mark_full_refresh_done(void);  // Отметить что full refresh выполнен

// C++ wrapper class for compatibility with existing code
class EInkDisplay {
public:
    EInkDisplay();
    ~EInkDisplay();

    void begin() { eink_init(); }

    SPIClass *spi();

    void refresh(bool partial) { 
        if (partial) eink_refresh(); 
        else eink_full_refresh();
    }

private:
    SPIClass *m_sd_spi = nullptr;
};

