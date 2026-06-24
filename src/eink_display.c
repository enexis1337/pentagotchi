/**
 * eink_display.c — драйвер для SSD1680 e-ink дисплея
 * WeAct 2.13" 250x122 монохромный
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "eink_display.h"
#include "pins.h"

static const char *TAG = "eink";

static spi_device_handle_t s_spi_device = NULL;
static uint8_t s_framebuffer[EINK_BUFFER_SIZE];
static bool s_initialized = false;

// Простой 8x8 шрифт (битовые маски для ASCII 32-126)
static const uint8_t font_8x8[95][8] = {
    // Пробел (32)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // ! (33)
    {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00},
    // " (34)
    {0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    // # (35)
    {0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00},
    // $ (36)
    {0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00},
    // Продолжение базового набора символов...
    // Для простоты добавим только основные символы
    // A (65)
    [33] = {0x1C, 0x36, 0x63, 0x7F, 0x63, 0x63, 0x63, 0x00}, // A
    // Остальные буквы можно добавить позже
};

// LUT (Look Up Table) для быстрого обновления (частичный режим)
static const unsigned char lut_full_update[] = {
    0x02, 0x02, 0x01, 0x11, 0x12, 0x12, 0x22, 0x22,
    0x66, 0x69, 0x69, 0x59, 0x58, 0x99, 0x99, 0x88,
    0x00, 0x00, 0x00, 0x00, 0xF8, 0xB4, 0x13, 0x51,
    0x35, 0x51, 0x51, 0x19, 0x01, 0x00
};

// ---------- Низкоуровневые SPI функции ----------

static esp_err_t eink_spi_write_cmd(uint8_t cmd)
{
    gpio_set_level(PIN_EINK_DC, 0); // Command mode
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    return spi_device_polling_transmit(s_spi_device, &t);
}

static esp_err_t eink_spi_write_data(const uint8_t *data, size_t len)
{
    if (len == 0) return ESP_OK;
    
    gpio_set_level(PIN_EINK_DC, 1); // Data mode
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    return spi_device_polling_transmit(s_spi_device, &t);
}

static esp_err_t eink_spi_write_data_byte(uint8_t data)
{
    return eink_spi_write_data(&data, 1);
}

static void eink_wait_busy(void)
{
    while (gpio_get_level(PIN_EINK_BUSY) == 1) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ---------- Инициализация последовательности для SSD1680 ----------

static esp_err_t eink_hw_reset(void)
{
    gpio_set_level(PIN_EINK_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_EINK_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

static esp_err_t eink_init_sequence(void)
{
    // Software reset
    eink_spi_write_cmd(SSD1680_SW_RESET);
    eink_wait_busy();

    // Driver output control
    eink_spi_write_cmd(SSD1680_DRIVER_OUTPUT_CONTROL);
    eink_spi_write_data_byte((EINK_HEIGHT - 1) & 0xFF);
    eink_spi_write_data_byte(((EINK_HEIGHT - 1) >> 8) & 0xFF);
    eink_spi_write_data_byte(0x00); // GD = 0; SM = 0; TB = 0;

    // Booster soft start
    eink_spi_write_cmd(SSD1680_BOOSTER_SOFT_START_CONTROL);
    eink_spi_write_data_byte(0xD7);
    eink_spi_write_data_byte(0xD6);
    eink_spi_write_data_byte(0x9D);

    // Write VCOM register
    eink_spi_write_cmd(SSD1680_WRITE_VCOM_REGISTER);
    eink_spi_write_data_byte(0xA8); // VCOM 7C

    // Set dummy line period
    eink_spi_write_cmd(SSD1680_SET_DUMMY_LINE_PERIOD);
    eink_spi_write_data_byte(0x1A); // 4 dummy lines per gate

    // Set gate time
    eink_spi_write_cmd(SSD1680_SET_GATE_TIME);
    eink_spi_write_data_byte(0x08); // 2us per line

    // Data entry mode
    eink_spi_write_cmd(SSD1680_DATA_ENTRY_MODE_SETTING);
    eink_spi_write_data_byte(0x03); // X increment; Y increment

    // Set Ram-X address start/end position
    eink_spi_write_cmd(SSD1680_SET_RAM_X_ADDRESS_START_END_POSITION);
    eink_spi_write_data_byte(0x00);
    eink_spi_write_data_byte((EINK_WIDTH / 8) - 1);

    // Set Ram-Y address start/end position
    eink_spi_write_cmd(SSD1680_SET_RAM_Y_ADDRESS_START_END_POSITION);
    eink_spi_write_data_byte(0x00);
    eink_spi_write_data_byte(0x00);
    eink_spi_write_data_byte((EINK_HEIGHT - 1) & 0xFF);
    eink_spi_write_data_byte((EINK_HEIGHT - 1) >> 8);

    // Border waveform
    eink_spi_write_cmd(SSD1680_BORDER_WAVEFORM_CONTROL);
    eink_spi_write_data_byte(0x05);

    // Load LUT
    eink_spi_write_cmd(SSD1680_WRITE_LUT_REGISTER);
    eink_spi_write_data(lut_full_update, sizeof(lut_full_update));

    ESP_LOGI(TAG, "SSD1680 initialization sequence completed");
    return ESP_OK;
}

// ---------- Public API ----------

esp_err_t eink_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    // Инициализация GPIO
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_EINK_CS) | (1ULL << PIN_EINK_DC) | (1ULL << PIN_EINK_RST),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    // BUSY pin как вход
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << PIN_EINK_BUSY);
    io_conf.pull_up_en = 1;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    // Инициализация SPI шины (если ещё не инициализирована)
    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_SPI_MISO,
        .mosi_io_num = PIN_SPI_MOSI,
        .sclk_io_num = PIN_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EINK_BUFFER_SIZE,
    };

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    // Настройка SPI device для e-ink
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 2 * 1000 * 1000, // 2 MHz для e-ink
        .mode = 0, // SPI mode 0
        .spics_io_num = PIN_EINK_CS,
        .queue_size = 7,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };

    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi_device);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device: %s", esp_err_to_name(ret));
        return ret;
    }

    // Hardware reset и инициализация дисплея
    eink_hw_reset();
    eink_init_sequence();

    // Очистка framebuffer'а
    memset(s_framebuffer, 0xFF, sizeof(s_framebuffer)); // 0xFF = белый для e-ink

    s_initialized = true;
    ESP_LOGI(TAG, "E-ink display initialized successfully");
    return ESP_OK;
}

esp_err_t eink_clear(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    memset(s_framebuffer, 0xFF, sizeof(s_framebuffer)); // Белый цвет
    return ESP_OK;
}

esp_err_t eink_print_text(const char *text, uint16_t x, uint16_t y)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!text) return ESP_ERR_INVALID_ARG;

    size_t len = strlen(text);
    for (size_t i = 0; i < len && (x + i * 8) < EINK_WIDTH; i++) {
        char c = text[i];
        
        // Простая реализация: только заглавные буквы и цифры
        if (c >= 'A' && c <= 'Z') {
            // Используем упрощенное отображение символов
            for (int row = 0; row < 8 && (y + row) < EINK_HEIGHT; row++) {
                uint8_t pattern = 0xFF; // Простые вертикальные линии для букв
                if (row == 0 || row == 3 || row == 7) pattern = 0x00; // Горизонтальные линии
                
                uint16_t byte_x = (x + i * 8) / 8;
                uint16_t bit_x = (x + i * 8) % 8;
                uint16_t byte_idx = ((y + row) * (EINK_WIDTH / 8)) + byte_x;
                
                if (byte_idx < EINK_BUFFER_SIZE) {
                    if (pattern & (0x80 >> bit_x)) {
                        s_framebuffer[byte_idx] &= ~(0x80 >> bit_x); // Чёрный пиксель
                    }
                }
            }
        } else if (c >= '0' && c <= '9') {
            // Простое отображение цифр
            for (int row = 0; row < 8 && (y + row) < EINK_HEIGHT; row++) {
                uint8_t pattern = 0xFF;
                if (row == 0 || row == 7 || row == 3) pattern = 0x00;
                
                uint16_t byte_x = (x + i * 8) / 8;
                uint16_t bit_x = (x + i * 8) % 8;
                uint16_t byte_idx = ((y + row) * (EINK_WIDTH / 8)) + byte_x;
                
                if (byte_idx < EINK_BUFFER_SIZE) {
                    if (pattern & (0x80 >> bit_x)) {
                        s_framebuffer[byte_idx] &= ~(0x80 >> bit_x);
                    }
                }
            }
        }
    }

    return ESP_OK;
}

esp_err_t eink_show_boot_status(const eink_status_t *status)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!status) return ESP_ERR_INVALID_ARG;

    eink_clear();

    // Заголовок
    eink_print_text("PENTAGOTCHI BOOT", 10, 10);
    
    // Имя устройства
    char name_buf[64];
    snprintf(name_buf, sizeof(name_buf), "NAME: %s", status->device_name);
    eink_print_text(name_buf, 10, 30);

    // Статус дисплея
    if (status->display_initialized) {
        eink_print_text("DISPLAY: OK", 10, 50);
    } else {
        eink_print_text("DISPLAY: FAIL", 10, 50);
    }

    // Статус SD карты
    if (status->sd_initialized) {
        char sd_buf[64];
        snprintf(sd_buf, sizeof(sd_buf), "SD: %luMB", status->sd_size_mb);
        eink_print_text(sd_buf, 10, 70);
    } else {
        eink_print_text("SD: NOT FOUND", 10, 70);
    }

    // Статусная строка
    eink_print_text("INITIALIZING...", 10, 100);

    return eink_refresh();
}

esp_err_t eink_refresh(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    // Set RAM X address counter
    eink_spi_write_cmd(SSD1680_SET_RAM_X_ADDRESS_COUNTER);
    eink_spi_write_data_byte(0x00);

    // Set RAM Y address counter
    eink_spi_write_cmd(SSD1680_SET_RAM_Y_ADDRESS_COUNTER);
    eink_spi_write_data_byte(0x00);
    eink_spi_write_data_byte(0x00);

    // Write RAM
    eink_spi_write_cmd(SSD1680_WRITE_RAM);
    eink_spi_write_data(s_framebuffer, EINK_BUFFER_SIZE);

    // Display update
    eink_spi_write_cmd(SSD1680_DISPLAY_UPDATE_CONTROL_2);
    eink_spi_write_data_byte(0xC4);

    eink_spi_write_cmd(SSD1680_MASTER_ACTIVATION);
    eink_spi_write_data_byte(0x01);

    eink_wait_busy();
    
    ESP_LOGI(TAG, "Display refreshed");
    return ESP_OK;
}

void eink_deinit(void)
{
    if (s_initialized) {
        // Deep sleep mode
        eink_spi_write_cmd(SSD1680_DEEP_SLEEP_MODE);
        eink_spi_write_data_byte(0x01);

        if (s_spi_device) {
            spi_bus_remove_device(s_spi_device);
            s_spi_device = NULL;
        }

        s_initialized = false;
        ESP_LOGI(TAG, "E-ink display deinitialized");
    }
}