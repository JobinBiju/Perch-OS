#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ---- pins (ES3C28P) ----
static constexpr int PIN_LCD_MOSI = 11, PIN_LCD_MISO = 13, PIN_LCD_SCLK = 12;
static constexpr int PIN_LCD_CS = 10, PIN_LCD_DC = 46, PIN_LCD_BL = 45;
static constexpr int PIN_TP_SDA = 16, PIN_TP_SCL = 15, PIN_TP_INT = 17, PIN_TP_RST = 18;
static constexpr int PIN_I2S_MCLK = 4, PIN_I2S_BCLK = 5, PIN_I2S_WS = 7, PIN_I2S_DOUT = 8, PIN_I2S_DIN = 6;
static constexpr int PIN_AMP_EN = 1, PIN_LED = 42;

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel; lgfx::Bus_SPI _bus; lgfx::Light_PWM _light; lgfx::Touch_FT5x06 _touch;
public:
  LGFX() {
    { auto c = _bus.config(); c.spi_host = SPI2_HOST; c.spi_mode = 0; c.freq_write = 80000000; c.freq_read = 16000000;
      c.spi_3wire = false; c.use_lock = true; c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = PIN_LCD_SCLK; c.pin_mosi = PIN_LCD_MOSI; c.pin_miso = PIN_LCD_MISO; c.pin_dc = PIN_LCD_DC;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c = _panel.config(); c.pin_cs = PIN_LCD_CS; c.pin_rst = -1; c.pin_busy = -1;
      c.panel_width = 240; c.panel_height = 320; c.offset_rotation = 0; c.readable = true; c.invert = true;
      c.rgb_order = false; c.bus_shared = false; _panel.config(c); }
    { auto c = _light.config(); c.pin_bl = PIN_LCD_BL; c.invert = false; c.freq = 44100; c.pwm_channel = 7;
      _light.config(c); _panel.setLight(&_light); }
    { auto c = _touch.config(); c.x_min = 0; c.x_max = 239; c.y_min = 0; c.y_max = 319;
      c.pin_int = PIN_TP_INT; c.pin_rst = PIN_TP_RST; c.bus_shared = false; c.offset_rotation = 2;
      c.i2c_port = 0; c.i2c_addr = 0x38; c.pin_sda = PIN_TP_SDA; c.pin_scl = PIN_TP_SCL; c.freq = 400000;
      _touch.config(c); _panel.setTouch(&_touch); }
    setPanel(&_panel);
  }
};
