#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#define EINK_WIDTH  250
#define EINK_HEIGHT 122

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
esp_err_t eink_set_rotation(int rotation);
void eink_deinit(void);
