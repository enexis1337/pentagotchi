#pragma once

// ---------- E-ink SPI bus (HSPI) ----------
#define PIN_EINK_MOSI       GPIO_NUM_15
#define PIN_EINK_SCK        GPIO_NUM_17

// ---------- E-ink (SSD1680, WeAct 2.13" 250x122) ----------
#define PIN_EINK_CS         GPIO_NUM_4
#define PIN_EINK_DC         GPIO_NUM_6
#define PIN_EINK_RES        GPIO_NUM_7
#define PIN_EINK_BUSY       GPIO_NUM_8

// ---------- SD card SPI bus (VSPI) ----------
#define PIN_SD_MOSI         GPIO_NUM_11
#define PIN_SD_MISO         GPIO_NUM_13
#define PIN_SD_SCK          GPIO_NUM_12
#define PIN_SD_CS           GPIO_NUM_5

// ---------- GPS (UART1) - TODO: implement GPS module support ----------
#define PIN_GPS_TX          GPIO_NUM_18   // ESP TX -> GPS RX
#define PIN_GPS_RX          GPIO_NUM_21            // ESP RX <- GPS TX
#define PIN_GPS_PTT         GPIO_NUM_16
#define GPS_UART_NUM         UART_NUM_1
#define GPS_BAUD_RATE        9600

// ---------- Battery monitoring - TODO: implement battery level reading ----------
#define PIN_BAT_ADC         GPIO_NUM_1    // ADC1_CH0, делитель 100k+100k

// ---------- Boot / strapping ----------
#define PIN_BOOT            GPIO_NUM_0    // pull-up 10k -> VCC

// ---------- WiFi scanning ----------
// промискуус режим не требует отдельных GPIO, использует встроенный radio
