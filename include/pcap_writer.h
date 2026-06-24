/**
 * pcap_writer.h — запись захваченных фреймов в формат PCAP на SD
 *
 * PCAP — стандартный формат, который понимает Wireshark/aircrack-ng/hashcat.
 * Один файл на BSSID, имя = MAC адрес точки.
 *
 * Путь к директории берётся из конфига (pwny.handshakes, по умолчанию
 * "/sdcard/handshakes" — как pwny.handshakes в оригинальном pwnagotchi).
 */

#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/**
 * Инициализация. Создаёт директорию dir_path, если её ещё нет.
 * dir_path обычно = config.pwny.handshakes_path.
 */
esp_err_t pcap_writer_init(const char *dir_path);

/**
 * Дописывает один 802.11 фрейм в pcap-файл, соответствующий bssid.
 * Если файла не существует — создаёт с pcap global header.
 */
esp_err_t pcap_writer_append_frame(const uint8_t bssid[6],
                                    const uint8_t *frame_data,
                                    size_t frame_len,
                                    uint32_t timestamp_us);

/**
 * Возвращает путь к pcap файлу для данного bssid (не проверяет существование).
 */
void pcap_writer_get_path(const uint8_t bssid[6], char *out_path, size_t out_size);
