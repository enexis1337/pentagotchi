/**
 * sd_card.h — инициализация SD через SPI (общая шина с e-ink)
 */

#pragma once
#include "esp_err.h"
#include <stdint.h>

/**
 * Информация о SD карте
 */
typedef struct {
    bool initialized;
    uint32_t size_mb;
    char type_name[16];
} sd_card_info_t;

/**
 * Монтирует SD карту в /sdcard.
 * Использует уже инициализированную SPI шину (см. spi_bus.h).
 */
esp_err_t sd_card_init(void);

/**
 * Получить информацию о SD карте
 */
esp_err_t sd_card_get_info(sd_card_info_t *info);

void sd_card_deinit(void);
