#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "u8g2.h"
#include "eink_display.h"
#include "pins.h"

static const char *TAG = "eink";

static spi_device_handle_t s_spi_device = NULL;
static uint8_t s_framebuffer[EINK_BUFFER_SIZE];
static bool s_initialized = false;
static bool s_power_on = false;

// u8g2 for text rendering
#define U8G2_TILE_W ((EINK_WIDTH + 7) / 8)
#define U8G2_TILE_H ((EINK_HEIGHT + 7) / 8)
#define U8G2_BUF_SIZE (U8G2_TILE_W * U8G2_TILE_H * 8)

static u8g2_t s_u8g2;
static uint8_t s_u8g2_buf[U8G2_BUF_SIZE];

static u8x8_display_info_t s_u8g2_display_info;

static void u8g2_init_draw(void)
{
    s_u8g2_display_info = {};
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

// ---------- Low-level SPI ----------

static esp_err_t eink_spi_write_cmd(uint8_t cmd)
{
    gpio_set_level(PIN_EINK_DC, 0);
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_buffer = &cmd;
    return spi_device_polling_transmit(s_spi_device, &t);
}

static esp_err_t eink_spi_write_data(const uint8_t *data, size_t len)
{
    if (len == 0) return ESP_OK;
    gpio_set_level(PIN_EINK_DC, 1);
    spi_transaction_t t = {};
    t.length = len * 8;
    t.tx_buffer = data;
    return spi_device_polling_transmit(s_spi_device, &t);
}

static void eink_spi_write_data_byte(uint8_t data)
{
    eink_spi_write_data(&data, 1);
}

// BUSY = HIGH when busy (board inverts SSD1680 active-low)
static void eink_wait_busy(void)
{
    int timeout = 500;
    while (gpio_get_level(PIN_EINK_BUSY) == 1 && timeout--) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ---------- Power ----------

static void eink_power_on(void)
{
    if (s_power_on) return;
    eink_spi_write_cmd(SSD1680_DISPLAY_UPDATE_CONTROL_2);
    eink_spi_write_data_byte(0xE0);
    eink_spi_write_cmd(SSD1680_MASTER_ACTIVATION);
    eink_wait_busy();
    s_power_on = true;
}

static void eink_power_off(void)
{
    if (!s_power_on) return;
    eink_spi_write_cmd(SSD1680_DISPLAY_UPDATE_CONTROL_2);
    eink_spi_write_data_byte(0x83);
    eink_spi_write_cmd(SSD1680_MASTER_ACTIVATION);
    eink_wait_busy();
    s_power_on = false;
}

// ---------- RAM area ----------

static void eink_set_ram_area(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    eink_spi_write_cmd(SSD1680_DATA_ENTRY_MODE_SETTING);
    eink_spi_write_data_byte(0x00);
    eink_spi_write_cmd(SSD1680_SET_RAM_X_ADDRESS_START_END_POSITION);
    eink_spi_write_data_byte(x / 8);
    eink_spi_write_data_byte((x + w - 1) / 8);
    eink_spi_write_cmd(SSD1680_SET_RAM_Y_ADDRESS_START_END_POSITION);
    eink_spi_write_data_byte(y & 0xFF);
    eink_spi_write_data_byte((y >> 8) & 0xFF);
    eink_spi_write_data_byte((y + h - 1) & 0xFF);
    eink_spi_write_data_byte(((y + h - 1) >> 8) & 0xFF);
    eink_spi_write_cmd(SSD1680_SET_RAM_X_ADDRESS_COUNTER);
    eink_spi_write_data_byte(x / 8);
    eink_spi_write_cmd(SSD1680_SET_RAM_Y_ADDRESS_COUNTER);
    eink_spi_write_data_byte(y & 0xFF);
    eink_spi_write_data_byte((y >> 8) & 0xFF);
}

static void eink_write_framebuffer(void)
{
    int chunk_rows = 8;
    for (int y = 0; y < EINK_HEIGHT; y += chunk_rows) {
        int n = chunk_rows;
        if (y + n > EINK_HEIGHT) n = EINK_HEIGHT - y;
        eink_set_ram_area(0, y, EINK_WIDTH, n);
        eink_spi_write_cmd(SSD1680_WRITE_RAM);
        esp_err_t ret = eink_spi_write_data(s_framebuffer + y * EINK_BYTES_PER_ROW, n * EINK_BYTES_PER_ROW);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI write failed at y=%d: %s", y, esp_err_to_name(ret));
            return;
        }
    }
}

// ---------- Init (GxEPD2 B74-based) ----------

static void eink_hw_reset(void)
{
    gpio_set_level(PIN_EINK_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_EINK_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

static void eink_init_display(void)
{
    eink_spi_write_cmd(SSD1680_SW_RESET);
    eink_wait_busy();
    vTaskDelay(pdMS_TO_TICKS(10));

    eink_spi_write_cmd(SSD1680_DRIVER_OUTPUT_CONTROL);
    eink_spi_write_data_byte((EINK_HEIGHT - 1) & 0xFF);
    eink_spi_write_data_byte(((EINK_HEIGHT - 1) >> 8) & 0xFF);
    eink_spi_write_data_byte(0x00);

    eink_spi_write_cmd(SSD1680_DATA_ENTRY_MODE_SETTING);
    eink_spi_write_data_byte(0x00);

    eink_spi_write_cmd(SSD1680_BORDER_WAVEFORM_CONTROL);
    eink_spi_write_data_byte(0x05);

    eink_spi_write_cmd(0x21);
    eink_spi_write_data_byte(0x00);
    eink_spi_write_data_byte(0x80);

    eink_spi_write_cmd(0x18);
    eink_spi_write_data_byte(0x80);

    eink_set_ram_area(0, 0, EINK_WIDTH, EINK_HEIGHT);
    s_power_on = false;
}

// ---------- Updates (GxEPD2 B74-based) ----------

static void eink_update_full(void)
{
    eink_power_on();
    eink_spi_write_cmd(SSD1680_DISPLAY_UPDATE_CONTROL_2);
    eink_spi_write_data_byte(0xF7);
    eink_spi_write_cmd(SSD1680_MASTER_ACTIVATION);
    eink_wait_busy();
    s_power_on = false;
}

static __attribute__((unused)) void eink_update_part(void)
{
    eink_power_on();
    eink_spi_write_cmd(SSD1680_DISPLAY_UPDATE_CONTROL_2);
    eink_spi_write_data_byte(0xFC);
    eink_spi_write_cmd(SSD1680_MASTER_ACTIVATION);
    eink_wait_busy();
}

// ---------- Public API ----------

esp_err_t eink_init(void)
{
    if (s_initialized) return ESP_OK;

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << PIN_EINK_CS) | (1ULL << PIN_EINK_DC) | (1ULL << PIN_EINK_RST);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << PIN_EINK_BUSY);
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

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
        return ret;
    }

    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = 2 * 1000 * 1000;
    devcfg.mode = 0;
    devcfg.spics_io_num = PIN_EINK_CS;
    devcfg.queue_size = 7;
    devcfg.flags = SPI_DEVICE_HALFDUPLEX;

    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi_device);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    eink_hw_reset();
    eink_init_display();

    memset(s_framebuffer, 0xFF, sizeof(s_framebuffer));
    u8g2_init_draw();
    s_initialized = true;

    eink_power_on();
    eink_set_ram_area(0, 0, EINK_WIDTH, EINK_HEIGHT);
    eink_write_framebuffer();
    eink_update_full();

    ESP_LOGI(TAG, "E-ink SSP1680 250x122 initialized");
    return ESP_OK;
}

esp_err_t eink_clear(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    memset(s_framebuffer, 0xFF, sizeof(s_framebuffer));
    return ESP_OK;
}

static void u8g2_buf_to_fb(void)
{
    memcpy(s_framebuffer, s_u8g2_buf, EINK_BUFFER_SIZE);

    // mask out columns beyond EINK_WIDTH in the last byte
    if (EINK_WIDTH % 8 != 0) {
        uint8_t invalid_mask = (uint8_t)(0xFFu >> (EINK_WIDTH % 8));
        for (int row = 0; row < EINK_HEIGHT; row++) {
            s_framebuffer[row * EINK_BYTES_PER_ROW + EINK_BYTES_PER_ROW - 1] |= invalid_mask;
        }
    }
}

esp_err_t eink_print_text(const char *text, uint16_t x, uint16_t y)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!text) return ESP_ERR_INVALID_ARG;

    memset(s_u8g2_buf, 0xFF, U8G2_BUF_SIZE);
    u8g2_SetFont(&s_u8g2, u8g2_font_7x13_tf);
    u8g2_SetFontPosTop(&s_u8g2);
    u8g2_DrawStr(&s_u8g2, x, y, text);
    u8g2_buf_to_fb();

    return ESP_OK;
}

esp_err_t eink_show_boot_status(const eink_status_t *status)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
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
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    eink_set_ram_area(0, 0, EINK_WIDTH, EINK_HEIGHT);
    eink_write_framebuffer();
    eink_update_full();

    ESP_LOGI(TAG, "Display refreshed");
    return ESP_OK;
}

void eink_deinit(void)
{
    if (s_initialized) {
        eink_power_off();
        eink_spi_write_cmd(SSD1680_DEEP_SLEEP_MODE);
        eink_spi_write_data_byte(0x01);
        if (s_spi_device) {
            spi_bus_remove_device(s_spi_device);
            s_spi_device = NULL;
        }
        s_initialized = false;
        s_power_on = false;
    }
}
