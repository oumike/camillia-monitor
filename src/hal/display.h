#pragma once

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <Wire.h>
#include <chsc6x.h>
#include <lgfx/v1/Touch.hpp>

#include "hardware.h"

class HeltecPanel : public lgfx::Panel_ST7789 {
protected:
    const uint8_t *getInitCommands(uint8_t listNumber) const override {
        static uint8_t gammaCommands[] = {0x26, 1, 0x01, 0xFF, 0xFF};
        if (listNumber == 1) {
            return gammaCommands;
        }
        return lgfx::Panel_ST7789::getInitCommands(listNumber);
    }
};

class HeltecTouch : public lgfx::ITouch {
public:
    HeltecTouch() {
        _cfg.i2c_addr = TOUCH_ADDR;
        _cfg.x_min = 0;
        _cfg.x_max = TFT_PANEL_WIDTH - 1;
        _cfg.y_min = 0;
        _cfg.y_max = TFT_PANEL_HEIGHT - 1;
    }

    bool init() override {
        if (_touch == nullptr) {
            _touch = new chsc6x(&Wire1, TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_RST);
        }
        _touch->chsc6x_init();
        return true;
    }

    uint_fast8_t getTouchRaw(lgfx::touch_point_t *point, uint_fast8_t count) override {
        if (point == nullptr || count == 0 || _touch == nullptr) {
            return 0;
        }

        uint16_t rawX = 0;
        uint16_t rawY = 0;
        if (_touch->chsc6x_read_touch_info(&rawX, &rawY) != 0) {
            point[0].size = 0;
            return 0;
        }

        int16_t x = static_cast<int16_t>(rawX);
        int16_t y = static_cast<int16_t>(rawY);
        if (x >= TFT_PANEL_WIDTH || y >= TFT_PANEL_HEIGHT) {
            x = static_cast<int16_t>(rawY);
            y = static_cast<int16_t>(rawX);
        }

        x = constrain(x, 0, TFT_PANEL_WIDTH - 1);
        y = constrain(y, 0, TFT_PANEL_HEIGHT - 1);
        point[0].x = x;
        point[0].y = y;
        point[0].size = 1;
        point[0].id = 1;
        return 1;
    }

    void wakeup() override {}
    void sleep() override {}

private:
    chsc6x *_touch = nullptr;
};

class HeltecDisplay : public lgfx::LGFX_Device {
public:
    HeltecDisplay() {
        {
            auto config = _bus.config();
            config.spi_host = TFT_SPI_HOST;
            config.spi_mode = 0;
            config.freq_write = TFT_SPI_WRITE_HZ;
            config.freq_read = TFT_SPI_READ_HZ;
            config.spi_3wire = TFT_SPI_3WIRE;
            config.use_lock = true;
            config.pin_sclk = TFT_SPI_SCK;
            config.pin_miso = TFT_SPI_MISO;
            config.pin_mosi = TFT_SPI_MOSI;
            config.pin_dc = TFT_DC;
            _bus.config(config);
            _panel.setBus(&_bus);
        }
        {
            auto config = _panel.config();
            config.pin_cs = TFT_CS;
            config.pin_rst = TFT_RST;
            config.panel_width = TFT_PANEL_WIDTH;
            config.panel_height = TFT_PANEL_HEIGHT;
            config.offset_x = TFT_PANEL_OFFSET_X;
            config.offset_y = TFT_PANEL_OFFSET_Y;
            config.invert = TFT_INVERT;
            config.rgb_order = TFT_RGB_ORDER;
            config.readable = false;
            _panel.config(config);
        }
        {
            auto config = _backlight.config();
            config.pin_bl = TFT_BL;
            config.invert = TFT_BL_INVERT;
            config.freq = TFT_BL_FREQ;
            config.pwm_channel = TFT_BL_PWM_CH;
            _backlight.config(config);
            _panel.setLight(&_backlight);
        }
        {
            auto config = _touch.config();
            config.x_min = 0;
            config.x_max = TFT_PANEL_WIDTH - 1;
            config.y_min = 0;
            config.y_max = TFT_PANEL_HEIGHT - 1;
            config.pin_int = TOUCH_INT;
            config.bus_shared = false;
            config.offset_rotation = 0;
            config.i2c_port = TOUCH_I2C_PORT;
            config.i2c_addr = TOUCH_ADDR;
            config.pin_sda = TOUCH_SDA;
            config.pin_scl = TOUCH_SCL;
            config.freq = 400000;
            _touch.config(config);
            _panel.setTouch(&_touch);
        }
        setPanel(&_panel);
    }

private:
    HeltecPanel _panel;
    lgfx::Bus_SPI _bus;
    lgfx::Light_PWM _backlight;
    HeltecTouch _touch;
};