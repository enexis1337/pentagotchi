#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "u8g2.h"
#include "eink_display.h"
#include "pins.h"

#include <Arduino.h>
#include <SPI.h>
#include "driver/spi_master.h"
#include "GxEPD2_213_GDEY0213B74.h"

static const char *TAG = "eink";

static GxEPD2_213_GDEY0213B74 *s_display = nullptr;
static SPIClass *s_spi = nullptr;
static int s_rotation = 0;
static int64_t s_last_full_refresh_us = 0;
static int64_t s_last_refresh_us = 0;
static uint32_t s_full_refresh_interval_sec = 600;

// u8g2 framebuffer 250x122 (user space)
u8g2_t g_u8g2;
uint8_t g_u8g2_buf[U8G2_BUF_SIZE];

// GxEPD2 framebuffer for rotated conversion
// GDEY0213B74: WIDTH=128 (16 bytes/row), HEIGHT=250
static const int GX_BYTES_PER_ROW = 16; // (128/8)
static const int GX_BUF_SIZE = GX_BYTES_PER_ROW * 250; // 4000 bytes
static uint8_t s_gx_buf[GX_BUF_SIZE];

static u8x8_display_info_t g_u8g2_display_info;

static void u8g2_init_draw(void)
{
    g_u8g2_display_info = (u8x8_display_info_t){};
    g_u8g2_display_info.tile_width = U8G2_TILE_W;
    g_u8g2_display_info.tile_height = U8G2_TILE_H;
    g_u8g2_display_info.pixel_width = EINK_WIDTH;
    g_u8g2_display_info.pixel_height = EINK_HEIGHT;

    u8x8_Setup(&g_u8g2.u8x8, u8x8_d_null_cb, u8x8_cad_empty, u8x8_byte_empty, u8x8_byte_empty);
    g_u8g2.u8x8.display_info = &g_u8g2_display_info;

    u8g2_SetupBuffer(&g_u8g2, g_u8g2_buf, U8G2_TILE_H, u8g2_ll_hvline_horizontal_right_lsb, U8G2_R0);

    u8g2_InitDisplay(&g_u8g2);
    u8g2_SetPowerSave(&g_u8g2, 0);

    memset(g_u8g2_buf, 0xFF, U8G2_BUF_SIZE);
    u8g2_SetDrawColor(&g_u8g2, 0);
}

// Convert u8g2 framebuffer (250x122 landscape, U8G2_R0) to GxEPD2 format
// GDEY0213B74 native: WIDTH=128 (16 bytes/row), HEIGHT=250 (portrait)
// Mapping: u8g2 pixel (ux, uy) → display column = uy, row = ux
static void u8g2_to_gxepd2(void)
{
    memset(s_gx_buf, 0xFF, GX_BUF_SIZE);

    for (int uy = 0; uy < EINK_HEIGHT; uy++) {
        int u8_row_offset = uy * 32; // u8g2: 32 bytes per row (250/8 rounded up)
        for (int ux = 0; ux < EINK_WIDTH; ux++) {
            int u8_idx = (ux >> 3) + u8_row_offset;
            int u8_bit = 7 - (ux & 7);
            if (g_u8g2_buf[u8_idx] & (1 << u8_bit)) {
                continue;
            }
            // Rotated mapping: gx = uy, gy = ux, horizontal flip
            int gy = EINK_HEIGHT - 1 - uy;
            int gx_idx = (gy >> 3) + ux * GX_BYTES_PER_ROW;
            int gx_bit = 7 - (gy & 7);
            if (gx_idx < GX_BUF_SIZE) {
                s_gx_buf[gx_idx] &= ~(1 << gx_bit);
            }
        }
    }
}

// ---------- Public API ----------

esp_err_t eink_init(void)
{
    if (s_display) return ESP_OK;

    // SPI bus config (shared with SD card)
    spi_bus_config_t buscfg = {};
    buscfg.miso_io_num = PIN_SPI_MISO;
    buscfg.mosi_io_num = PIN_SPI_MOSI;
    buscfg.sclk_io_num = PIN_SPI_SCK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = 8192;

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    // Create Arduino SPIClass on the same host (reuses existing bus)
    s_spi = new SPIClass(SPI2_HOST);
    s_spi->begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, -1);

    // Create GxEPD2 display instance
    s_display = new GxEPD2_213_GDEY0213B74(
        PIN_EINK_CS, PIN_EINK_DC, PIN_EINK_RST, PIN_EINK_BUSY
    );

    s_display->selectSPI(*s_spi, SPISettings(4000000, MSBFIRST, SPI_MODE0));
    s_display->init(0); // serial_diag_bitrate = 0 (disabled)

    // Init u8g2 for text rendering
    u8g2_init_draw();

    ESP_LOGI(TAG, "E-ink GDEY0213B74 (SSD1680) %dx%d initialized (GxEPD2)", EINK_WIDTH, EINK_HEIGHT);
    return ESP_OK;
}

esp_err_t eink_clear(void)
{
    if (!s_display) return ESP_ERR_INVALID_STATE;
    memset(g_u8g2_buf, 0xFF, U8G2_BUF_SIZE);
    return ESP_OK;
}

esp_err_t eink_print_text(const char *text, uint16_t x, uint16_t y)
{
    if (!s_display) return ESP_ERR_INVALID_STATE;
    if (!text) return ESP_ERR_INVALID_ARG;

    u8g2_SetFont(&g_u8g2, u8g2_font_7x13_tf);
    u8g2_SetFontPosTop(&g_u8g2);
    u8g2_DrawStr(&g_u8g2, x, y, text);

    return ESP_OK;
}

esp_err_t eink_show_boot_status(const eink_status_t *status)
{
    if (!s_display) return ESP_ERR_INVALID_STATE;
    if (!status) return ESP_ERR_INVALID_ARG;

    eink_clear();

    eink_print_text("PENTAGOTCHI BOOT", 10, 10);
    char buf[64];
    snprintf(buf, sizeof(buf), "NAME: %s", status->device_name);
    eink_print_text(buf, 10, 25);
    eink_print_text(status->display_initialized ? "DISPLAY: OK" : "DISPLAY: FAIL", 10, 40);

    if (status->sd_initialized) {
        snprintf(buf, sizeof(buf), "SD: %luMB", (unsigned long)status->sd_size_mb);
        eink_print_text(buf, 10, 55);
    } else {
        eink_print_text("SD: NOT FOUND", 10, 55);
    }

    eink_print_text("INITIALIZING...", 10, 70);

    return eink_refresh();
}

esp_err_t eink_init_partial(void)
{
    if (!s_display) return ESP_ERR_INVALID_STATE;
    ESP_LOGD(TAG, "Partial mode: handled by GxEPD2 natively");
    return ESP_OK;
}

esp_err_t eink_refresh(void)
{
    if (!s_display) return ESP_ERR_INVALID_STATE;

    int64_t now = esp_timer_get_time();
    if (now - s_last_refresh_us < 500000) {
        return ESP_OK;
    }

    u8g2_to_gxepd2();
    s_display->writeImage(s_gx_buf, 0, 0, 122, 250);
    s_display->refresh(true); // partial update

    s_last_refresh_us = now;
    ESP_LOGD(TAG, "Display partial refresh (GxEPD2)");
    return ESP_OK;
}

esp_err_t eink_full_refresh(void)
{
    if (!s_display) return ESP_ERR_INVALID_STATE;

    u8g2_to_gxepd2();
    s_display->writeImageForFullRefresh(s_gx_buf, 0, 0, 122, 250);
    s_display->refresh(false); // full update with flash

    ESP_LOGI(TAG, "Display full refresh (anti-ghosting)");
    return ESP_OK;
}

esp_err_t eink_set_rotation(int rotation)
{
    s_rotation = rotation;
    if (rotation == 180) {
        u8g2_SetDisplayRotation(&g_u8g2, U8G2_R2);
    } else {
        u8g2_SetDisplayRotation(&g_u8g2, U8G2_R0);
    }
    ESP_LOGI(TAG, "Rotation set to %d", rotation);
    return ESP_OK;
}

void eink_deinit(void)
{
    if (s_display) {
        s_display->hibernate();
        s_display = nullptr;
    }
    if (s_spi) {
        s_spi->end();
        s_spi = nullptr;
    }
}

// ========== Управление режимом обновления ==========

void eink_set_full_refresh_interval(uint32_t seconds) {
    s_full_refresh_interval_sec = seconds;
    ESP_LOGI(TAG, "Full refresh interval set to %lu seconds", (unsigned long)seconds);
}

bool eink_should_do_full_refresh(void) {
    if (s_last_full_refresh_us == 0) {
        return true;
    }

    int64_t now_us = esp_timer_get_time();
    int64_t elapsed_sec = (now_us - s_last_full_refresh_us) / 1000000;

    return elapsed_sec >= s_full_refresh_interval_sec;
}

void eink_mark_full_refresh_done(void) {
    s_last_full_refresh_us = esp_timer_get_time();
    ESP_LOGD(TAG, "Full refresh timestamp updated");
}
