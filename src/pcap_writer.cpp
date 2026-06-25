/**
 * pcap_writer.c — запись фреймов в PCAP формат
 *
 * PCAP Global Header (24 байта) + per-packet header (16 байт) + raw frame.
 * LinkType для 802.11 raw = 105 (DLT_IEEE802_11)
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "pcap_writer.h"

static const char *TAG = "pcap_writer";
#define DLT_IEEE802_11 105
#define MAX_DIR_LEN 96

static char s_dir_path[MAX_DIR_LEN] = "/sdcard/handshakes"; // дефолт на случай если init не вызван

typedef struct __attribute__((packed)) {
    uint32_t magic_number;   // 0xa1b2c3d4
    uint16_t version_major;  // 2
    uint16_t version_minor;  // 4
    int32_t  thiszone;       // GMT offset, 0
    uint32_t sigfigs;        // 0
    uint32_t snaplen;        // max length captured packets
    uint32_t network;        // data link type
} pcap_global_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;  // bytes saved
    uint32_t orig_len;  // bytes originally on wire
} pcap_packet_hdr_t;

esp_err_t pcap_writer_init(const char *dir_path)
{
    if (dir_path && dir_path[0] != '\0') {
        strncpy(s_dir_path, dir_path, MAX_DIR_LEN - 1);
    }

    struct stat st;
    if (stat(s_dir_path, &st) != 0) {
        if (mkdir(s_dir_path, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create %s", s_dir_path);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Created handshakes directory: %s", s_dir_path);
    }
    return ESP_OK;
}

void pcap_writer_get_path(const uint8_t bssid[6], char *out_path, size_t out_size)
{
    snprintf(out_path, out_size, "%s/%02X%02X%02X%02X%02X%02X.pcap",
             s_dir_path, bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
}

esp_err_t pcap_writer_append_frame(const uint8_t bssid[6],
                                    const uint8_t *frame_data,
                                    size_t frame_len,
                                    uint32_t timestamp_us)
{
    char path[160];
    pcap_writer_get_path(bssid, path, sizeof(path));

    struct stat st;
    bool file_exists = (stat(path, &st) == 0);

    FILE *f = fopen(path, file_exists ? "ab" : "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s", path);
        return ESP_FAIL;
    }

    if (!file_exists) {
        pcap_global_hdr_t gh = {
            .magic_number = 0xa1b2c3d4,
            .version_major = 2,
            .version_minor = 4,
            .thiszone = 0,
            .sigfigs = 0,
            .snaplen = 65535,
            .network = DLT_IEEE802_11,
        };
        fwrite(&gh, sizeof(gh), 1, f);
        ESP_LOGI(TAG, "Created new pcap: %s", path);
    }

    pcap_packet_hdr_t ph = {
        .ts_sec = (uint32_t)(timestamp_us / 1000000),
        .ts_usec = (uint32_t)(timestamp_us % 1000000),
        .incl_len = (uint32_t)frame_len,
        .orig_len = (uint32_t)frame_len,
    };
    fwrite(&ph, sizeof(ph), 1, f);
    fwrite(frame_data, frame_len, 1, f);

    fclose(f);
    return ESP_OK;
}
