// Display Library for SPI e-paper panels from Dalian Good Display and boards from Waveshare.
// Requires HW SPI and Adafruit_GFX. Caution: the e-paper panels require 3.3V supply AND data lines!
//
// Display Library based on Demo Example from Good Display: https://www.good-display.com/companyfile/32/
//
// Author: Jean-Marc Zingg
//
// Version: see library.properties
//
// Library: https://github.com/ZinggJM/GxEPD2

#ifndef _GxEPD2_EPD_H_
#define _GxEPD2_EPD_H_

#include <Arduino.h>
#include <SPI.h>

#include <GxEPD2.h>

#pragma GCC diagnostic ignored "-Wunused-parameter"

class GxEPD2_EPD
{
  public:
    const uint16_t WIDTH;
    const uint16_t HEIGHT;
    const GxEPD2::Panel panel;
    const bool hasColor;
    const bool hasPartialUpdate;
    const bool hasFastPartialUpdate;
    GxEPD2_EPD(int16_t cs, int16_t dc, int16_t rst, int16_t busy, int16_t busy_level, uint32_t busy_timeout,
               uint16_t w, uint16_t h, GxEPD2::Panel p, bool c, bool pu, bool fpu);
    virtual void init(uint32_t serial_diag_bitrate = 0);
    virtual void init(uint32_t serial_diag_bitrate, bool initial, uint16_t reset_duration = 10, bool pulldown_rst_mode = false);
    virtual void end();
    virtual void clearScreen(uint8_t value) = 0;
    virtual void writeScreenBuffer(uint8_t value) = 0;
    virtual void writeImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false) = 0;
    virtual void writeImageForFullRefresh(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false)
    {
      writeImage(bitmap, x, y, w, h, invert, mirror_y, pgm);
    }
    virtual void writeImageToPrevious(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false) {};
    virtual void writeImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                                int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false) = 0;
    virtual void writeScreenBufferAgain(uint8_t value = 0xFF)
    {
      writeScreenBuffer(value);
    }
    virtual void writeImageAgain(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false)
    {
      writeImage(bitmap, x, y, w, h, invert, mirror_y, pgm);
    }
    virtual void writeImagePartAgain(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                                     int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false)
    {
      writeImagePart(bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
    }
    virtual void writeDemoBitmap(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, int16_t mode = 0, bool mirror_y = false, bool pgm = false) {};
    virtual void drawDemoBitmap(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, int16_t mode = 0, bool mirror_y = false, bool pgm = false) {};
    virtual void refresh(bool partial_update_mode = false) = 0;
    virtual void refresh(int16_t x, int16_t y, int16_t w, int16_t h) = 0;
    virtual void powerOff() = 0;
    virtual void hibernate() = 0;
    virtual void setPaged() {};
    virtual void selectFastFullUpdate(bool) {};
    virtual void drawNativeColors() {};
    void setBusyCallback(void (*busyCallback)(const void*), const void* busy_callback_parameter = 0);
    void enableRead(int16_t sck, int16_t mosi);
    static inline uint16_t gx_uint16_min(uint16_t a, uint16_t b)
    {
      return (a < b ? a : b);
    };
    static inline uint16_t gx_uint16_max(uint16_t a, uint16_t b)
    {
      return (a > b ? a : b);
    };
    void selectSPI(SPIClass& spi, SPISettings spi_settings);
  protected:
    void _reset();
    void _waitWhileBusy(const char* comment = 0, uint16_t busy_time = 5000);
    void _writeCommand(uint8_t c);
    void _writeData(uint8_t d);
    void _writeData(const uint8_t* data, uint16_t n);
    void _writeDataPGM(const uint8_t* data, uint16_t n, int16_t fill_with_zeroes = 0);
    void _writeDataPGM_sCS(const uint8_t* data, uint16_t n, int16_t fill_with_zeroes = 0);
    void _writeCommandData(const uint8_t* pCommandData, uint8_t datalen);
    void _writeCommandDataPGM(const uint8_t* pCommandData, uint8_t datalen);
    void _startTransfer();
    void _transfer(uint8_t value);
    void _endTransfer();
    uint8_t _readData();
    void _readData(uint8_t* data, uint16_t n);
  protected:
    int16_t _cs, _dc, _rst, _busy, _busy_level;
    int16_t _sck, _mosi;
    uint32_t _busy_timeout;
    bool _diag_enabled, _pulldown_rst_mode;
    SPIClass* _pSPIx;
    SPISettings _spi_settings;
    bool _initial_write, _initial_refresh;
    bool _power_is_on, _using_partial_mode, _hibernating;
    bool _init_display_done;
    uint16_t _reset_duration;
    void (*_busy_callback)(const void*);
    const void* _busy_callback_parameter;
#include <GxEPD2_EPD_friends.h>
};

#endif
