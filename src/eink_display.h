#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#define EINK_WIDTH  250
#define EINK_HEIGHT 122
#define EINK_BYTES_PER_ROW ((EINK_WIDTH + 7) / 8)
#define EINK_BUFFER_SIZE (EINK_BYTES_PER_ROW * EINK_HEIGHT)

// SSD1680 commands
#define SSD1680_SW_RESET                    0x12
#define SSD1680_DRIVER_OUTPUT_CONTROL       0x01
#define SSD1680_DATA_ENTRY_MODE_SETTING     0x11
#define SSD1680_WRITE_RAM                   0x24
#define SSD1680_BORDER_WAVEFORM_CONTROL     0x3C
#define SSD1680_SET_RAM_X_ADDRESS_START_END_POSITION 0x44
#define SSD1680_SET_RAM_Y_ADDRESS_START_END_POSITION 0x45
#define SSD1680_SET_RAM_X_ADDRESS_COUNTER   0x4E
#define SSD1680_SET_RAM_Y_ADDRESS_COUNTER   0x4F
#define SSD1680_DISPLAY_UPDATE_CONTROL_2    0x22
#define SSD1680_MASTER_ACTIVATION           0x20
#define SSD1680_DEEP_SLEEP_MODE             0x10

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
esp_err_t eink_refresh(void);
void eink_deinit(void);
