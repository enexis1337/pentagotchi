/**
 * sd_card.c — монтирование SD через SPI (esp-idf sdspi driver)
 */

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "sd_card.h"
#include "pins.h"

static const char *TAG = "sd_card";
static sdmmc_card_t *s_card = NULL;
static bool s_initialized = false;

#define MOUNT_POINT "/sdcard"

esp_err_t sd_card_init(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST; // тот же SPI host, что у e-ink — шина общая

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_SD_CS;
    slot_config.host_id = host.slot;

    esp_err_t ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config,
                                             &mount_config, &s_card);

    if (ret != ESP_OK) {
        s_initialized = false;
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. Card may need formatting.");
        } else {
            ESP_LOGE(TAG, "Failed to init SD card (%s). Check wiring/pull-ups.", esp_err_to_name(ret));
        }
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

esp_err_t sd_card_get_info(sd_card_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;

    info->initialized = s_initialized;
    
    if (s_initialized && s_card) {
        // Размер в мегабайтах
        uint64_t card_size = ((uint64_t) s_card->csd.capacity) * s_card->csd.sector_size;
        info->size_mb = (uint32_t)(card_size / (1024 * 1024));
        
        // Тип карты
        switch (s_card->ocr & SD_OCR_SDHC_CAP) {
            case SD_OCR_SDHC_CAP:
                if (s_card->ocr & SD_OCR_XC_CAP) {
                    strncpy(info->type_name, "SDXC", sizeof(info->type_name) - 1);
                } else {
                    strncpy(info->type_name, "SDHC", sizeof(info->type_name) - 1);
                }
                break;
            default:
                strncpy(info->type_name, "SDSC", sizeof(info->type_name) - 1);
                break;
        }
        info->type_name[sizeof(info->type_name) - 1] = '\0';
    } else {
        info->size_mb = 0;
        strncpy(info->type_name, "NONE", sizeof(info->type_name) - 1);
        info->type_name[sizeof(info->type_name) - 1] = '\0';
    }

    return ESP_OK;
}

void sd_card_deinit(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
        s_card = NULL;
        s_initialized = false;
        ESP_LOGI(TAG, "SD card unmounted");
    }
}
