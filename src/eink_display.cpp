#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "u8g2.h"
#include "lib_ssd1680.h"
#include "ssd1680_fonts.h"
#include "eink_display.h"
#include "pins.h"

static const char *TAG = "eink";

static ssd1680_t *s_disp = NULL;
static int s_rotation = 0; // 0/90/180/270 — user-facing rotation

// u8g2 framebuffer 250x122 (user space)
#define U8G2_TILE_W ((EINK_WIDTH + 7) / 8)
#define U8G2_TILE_H ((EINK_HEIGHT + 7) / 8)
#define U8G2_BUF_SIZE (U8G2_TILE_W * U8G2_TILE_H * 8)

static u8g2_t s_u8g2;
static uint8_t s_u8g2_buf[U8G2_BUF_SIZE];

static u8x8_display_info_t s_u8g2_display_info;

static void u8g2_init_draw(void)
{
    s_u8g2_display_info = (u8x8_display_info_t){};
    s_u8g2_display_info.tile_width = U8G2_TILE_W;
    s_u8g2_display_info.tile_height = U8G2_TILE_H;
    s_u8g2_display_info.pixel_width = EINK_WIDTH;
    s_u8g2_display_info.pixel_height = EINK_HEIGHT;

    u8x8_Setup(&s_u8g2.u8x8, u8x8_d_null_cb, u8x8_cad_empty, u8x8_byte_empty, u8x8_byte_empty);
    s_u8g2.u8x8.display_info = &s_u8g2_display_info;

    u8g2_SetupBuffer(&s_u8g2, s_u8g2_buf, U8G2_TILE_H, u8g2_ll_hvline_horizontal_right_lsb, U8G2_R0);

    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);

    memset(s_u8g2_buf, 0xFF, U8G2_BUF_SIZE);
    u8g2_SetDrawColor(&s_u8g2, 0);
}

// Copy u8g2 buffer (250x122) into ssd1680 library framebuffer (122x250 NORMAL)
// Mapping depends on s_rotation (0/90/180/270)
static void u8g2_to_ssd1680_fb(void)
{
    if (!s_disp) return;

    static const int fw = EINK_WIDTH;   // 250
    static const int fh = EINK_HEIGHT;  // 122

    uint8_t *lib_bw = s_disp->framebuffer_bw;
    uint8_t *lib_red = s_disp->framebuffer_red;
    int fb_size = s_disp->framebuffer_size;

    memset(lib_bw, 0xFF, fb_size);
    memset(lib_red, 0xFF, fb_size);

    for (int uy = 0; uy < fh; uy++) {
        for (int ux = 0; ux < fw; ux++) {
            int u8_idx = (ux >> 3) + uy * ((fw + 7) / 8);
            int u8_bit = 7 - (ux & 7);

            if (s_u8g2_buf[u8_idx] & (1 << u8_bit)) {
                continue; // white
            }

            int lx, ly;
            switch (s_rotation) {
                case 90:
                    lx = ux;
                    ly = uy;
                    break;
                case 180:
                    lx = (fh - 1) - uy;
                    ly = ux;
                    break;
                case 270:
                    lx = (fh - 1) - ux;
                    ly = (fw - 1) - uy;
                    break;
                default: // 0
                    lx = uy;
                    ly = (fw - 1) - ux;
                    break;
            }

            int lib_idx = (lx >> 3) + ly * s_disp->clmn_cnt;
            int lib_bit = 7 - (lx & 7);
            if (lib_idx < fb_size) {
                lib_bw[lib_idx] &= ~(1 << lib_bit);
            }
        }
    }
}

// ---------- Public API ----------

esp_err_t eink_init(void)
{
    if (s_disp) return ESP_OK;

    // SPI bus config
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

    // Pin map
    ssd1680_pinmap_t pinmap = {
        .busy = PIN_EINK_BUSY,
        .reset = PIN_EINK_RST,
        .dc = PIN_EINK_DC,
        .cs = PIN_EINK_CS,
    };

    // Init SSD1680 library (res_x=122, res_y=250, NORMAL orientation)
    s_disp = ssd1680_init(SPI2_HOST, pinmap, 122, 250, SSD1680_NORMAL);
    if (!s_disp) {
        ESP_LOGE(TAG, "ssd1680_init failed");
        return ESP_FAIL;
    }

    // Init u8g2 for text rendering
    u8g2_init_draw();

    ESP_LOGI(TAG, "E-ink SSD1680 %dx%d initialized (lib_ssd1680)", EINK_WIDTH, EINK_HEIGHT);
    return ESP_OK;
}

esp_err_t eink_clear(void)
{
    if (!s_disp) return ESP_ERR_INVALID_STATE;

    memset(s_u8g2_buf, 0xFF, U8G2_BUF_SIZE);
    u8g2_to_ssd1680_fb();

    return ESP_OK;
}

esp_err_t eink_print_text(const char *text, uint16_t x, uint16_t y)
{
    if (!s_disp) return ESP_ERR_INVALID_STATE;
    if (!text) return ESP_ERR_INVALID_ARG;

    u8g2_SetFont(&s_u8g2, u8g2_font_7x13_tf);
    u8g2_SetFontPosTop(&s_u8g2);
    u8g2_DrawStr(&s_u8g2, x, y, text);

    return ESP_OK;
}

esp_err_t eink_show_boot_status(const eink_status_t *status)
{
    if (!s_disp) return ESP_ERR_INVALID_STATE;
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

esp_err_t eink_refresh(void)
{
    if (!s_disp) return ESP_ERR_INVALID_STATE;

    // Convert u8g2 buffer → ssd1680 framebuffer
    u8g2_to_ssd1680_fb();

    // Send framebuffer to display
    ssd1680_send_framebuffer(s_disp);

    // Full refresh
    ssd1680_refresh(s_disp, FULL_REFRESH);

    ESP_LOGI(TAG, "Display refreshed");
    return ESP_OK;
}

esp_err_t eink_set_rotation(int rotation)
{
    s_rotation = rotation;
    ESP_LOGI(TAG, "Rotation set to %d", rotation);
    return ESP_OK;
}

void eink_deinit(void)
{
    if (s_disp) {
        ssd1680_sleep(s_disp);
        ssd1680_deinit(s_disp);
        s_disp = NULL;
    }
}
