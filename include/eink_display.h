/**
 * eink_display.h — управление e-ink дисплеем SSD1680
 * WeAct 2.13" 250x122 монохромный дисплей
 *
 * Основные функции:
 * - Инициализация SPI и дисплея
 * - Базовый вывод текста
 * - Статус экран при загрузке
 */

#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// Размеры дисплея WeAct 2.13"
#define EINK_WIDTH  250
#define EINK_HEIGHT 122
#define EINK_BUFFER_SIZE ((EINK_WIDTH * EINK_HEIGHT) / 8)

// Команды SSD1680
#define SSD1680_SW_RESET                    0x12
#define SSD1680_DRIVER_OUTPUT_CONTROL       0x01
#define SSD1680_BOOSTER_SOFT_START_CONTROL  0x0C
#define SSD1680_GATE_SCAN_START_POSITION    0x0F
#define SSD1680_DEEP_SLEEP_MODE             0x10
#define SSD1680_DATA_ENTRY_MODE_SETTING     0x11
#define SSD1680_WRITE_RAM                   0x24
#define SSD1680_WRITE_VCOM_REGISTER         0x2C
#define SSD1680_WRITE_LUT_REGISTER          0x32
#define SSD1680_SET_DUMMY_LINE_PERIOD       0x3A
#define SSD1680_SET_GATE_TIME               0x3B
#define SSD1680_BORDER_WAVEFORM_CONTROL     0x3C
#define SSD1680_SET_RAM_X_ADDRESS_START_END_POSITION 0x44
#define SSD1680_SET_RAM_Y_ADDRESS_START_END_POSITION 0x45
#define SSD1680_SET_RAM_X_ADDRESS_COUNTER   0x4E
#define SSD1680_SET_RAM_Y_ADDRESS_COUNTER   0x4F
#define SSD1680_TERMINATE_FRAME_READ_WRITE  0xFF
#define SSD1680_DISPLAY_UPDATE_CONTROL_2    0x22
#define SSD1680_MASTER_ACTIVATION           0x20

/**
 * Статус инициализации компонентов системы
 */
typedef struct {
    bool sd_initialized;
    uint32_t sd_size_mb;
    bool display_initialized;
    char device_name[32];
} eink_status_t;

/**
 * Инициализация SPI шины и e-ink дисплея
 */
esp_err_t eink_init(void);

/**
 * Очистка дисплея (заливка белым)
 */
esp_err_t eink_clear(void);

/**
 * Отображение статуса загрузки системы
 */
esp_err_t eink_show_boot_status(const eink_status_t *status);

/**
 * Простой вывод текста (для отладки)
 */
esp_err_t eink_print_text(const char *text, uint16_t x, uint16_t y);

/**
 * Обновление дисплея (применение изменений)
 */
esp_err_t eink_refresh(void);

/**
 * Деинициализация и переход в deep sleep
 */
void eink_deinit(void);