#pragma once
#include <cstdint>
#include <cstddef>
#include "hardware/i2c.h"

class OledDisplay {
public:
    static constexpr uint8_t WIDTH = 128;
    static constexpr uint8_t HEIGHT = 32;
    static constexpr uint8_t PAGES = HEIGHT / 8;
    static constexpr uint16_t BUFFER_SIZE = WIDTH * PAGES;

    // SSD1306 via hardware I2C on GPIOs available on the standard Pico.
    static constexpr uint8_t PIN_SDA = 27;
    static constexpr uint8_t PIN_SCL = 28;
    static constexpr uint8_t I2C_ADDR = 0x3C;
    static constexpr uint32_t I2C_BAUD = 400000;
    static constexpr i2c_inst_t* I2C_PORT = i2c1;

    void init();
    void clear();
    void update();
    void set_dirty() { dirty_ = true; }
    bool dirty() const { return dirty_; }

    void draw_text(int x, int y, const char* text, bool invert = false);
    void draw_text_small(int x, int y, const char* text, bool invert = false);
    void draw_hline(int x, int y, int w, bool on = true);
    void draw_pixel(int x, int y, bool on = true);
    void fill_rect(int x, int y, int w, int h, bool on = true);
    void draw_text_right(int right_x, int y, const char* text, bool invert = false);

private:
    uint8_t buffer_[BUFFER_SIZE] = {};
    bool initialized_ = false;
    bool dirty_ = true;

    void send_command(uint8_t cmd);
    void send_command_list(const uint8_t* cmds, size_t n);
    void send_data(const uint8_t* data, size_t n);
    void draw_char_5x7(int x, int y, char c, bool invert);
    static const uint8_t* glyph_5x7(char c);
};
