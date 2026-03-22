#include "oled_display.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include <cstring>

namespace {
static const uint8_t GLYPH_UNKNOWN[5] = {0x7E,0x42,0x5A,0x42,0x7E};
static const uint8_t GLYPH_SPACE[5]   = {0,0,0,0,0};

struct GlyphMap { char c; uint8_t b[5]; };
static const GlyphMap kGlyphs[] = {
{' ',{0,0,0,0,0}}, {'-',{0x08,0x08,0x08,0x08,0x08}}, {'_',{0x40,0x40,0x40,0x40,0x40}},
{':',{0x00,0x36,0x36,0x00,0x00}}, {'.',{0x00,0x60,0x60,0x00,0x00}}, {'/',{0x20,0x10,0x08,0x04,0x02}},
{'0',{0x3E,0x51,0x49,0x45,0x3E}}, {'1',{0x00,0x42,0x7F,0x40,0x00}}, {'2',{0x42,0x61,0x51,0x49,0x46}},
{'3',{0x21,0x41,0x45,0x4B,0x31}}, {'4',{0x18,0x14,0x12,0x7F,0x10}}, {'5',{0x27,0x45,0x45,0x45,0x39}},
{'6',{0x3C,0x4A,0x49,0x49,0x30}}, {'7',{0x01,0x71,0x09,0x05,0x03}}, {'8',{0x36,0x49,0x49,0x49,0x36}},
{'9',{0x06,0x49,0x49,0x29,0x1E}},
{'A',{0x7E,0x11,0x11,0x11,0x7E}}, {'B',{0x7F,0x49,0x49,0x49,0x36}}, {'C',{0x3E,0x41,0x41,0x41,0x22}},
{'D',{0x7F,0x41,0x41,0x22,0x1C}}, {'E',{0x7F,0x49,0x49,0x49,0x41}}, {'F',{0x7F,0x09,0x09,0x09,0x01}},
{'G',{0x3E,0x41,0x49,0x49,0x7A}}, {'H',{0x7F,0x08,0x08,0x08,0x7F}}, {'I',{0x00,0x41,0x7F,0x41,0x00}},
{'J',{0x20,0x40,0x41,0x3F,0x01}}, {'K',{0x7F,0x08,0x14,0x22,0x41}}, {'L',{0x7F,0x40,0x40,0x40,0x40}},
{'M',{0x7F,0x02,0x0C,0x02,0x7F}}, {'N',{0x7F,0x04,0x08,0x10,0x7F}}, {'O',{0x3E,0x41,0x41,0x41,0x3E}},
{'P',{0x7F,0x09,0x09,0x09,0x06}}, {'Q',{0x3E,0x41,0x51,0x21,0x5E}}, {'R',{0x7F,0x09,0x19,0x29,0x46}},
{'S',{0x46,0x49,0x49,0x49,0x31}}, {'T',{0x01,0x01,0x7F,0x01,0x01}}, {'U',{0x3F,0x40,0x40,0x40,0x3F}},
{'V',{0x1F,0x20,0x40,0x20,0x1F}}, {'W',{0x3F,0x40,0x38,0x40,0x3F}}, {'X',{0x63,0x14,0x08,0x14,0x63}},
{'Y',{0x07,0x08,0x70,0x08,0x07}}, {'Z',{0x61,0x51,0x49,0x45,0x43}},
};
}

const uint8_t* OledDisplay::glyph_5x7(char c) {
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    for (const auto& g : kGlyphs) if (g.c == c) return g.b;
    return (c == ' ') ? GLYPH_SPACE : GLYPH_UNKNOWN;
}

void OledDisplay::send_command(uint8_t cmd) {
    uint8_t pkt[2] = {0x00, cmd};
    i2c_write_blocking(I2C_PORT, I2C_ADDR, pkt, 2, false);
}

void OledDisplay::send_command_list(const uint8_t* cmds, size_t n) {
    if (!cmds || n == 0) return;
    for (size_t i = 0; i < n; ++i) send_command(cmds[i]);
}

void OledDisplay::send_data(const uint8_t* data, size_t n) {
    if (!data || n == 0) return;
    uint8_t pkt[17];
    pkt[0] = 0x40;
    size_t i = 0;
    while (i < n) {
        size_t chunk = (n - i > 16) ? 16 : (n - i);
        std::memcpy(&pkt[1], &data[i], chunk);
        i2c_write_blocking(I2C_PORT, I2C_ADDR, pkt, chunk + 1, false);
        i += chunk;
    }
}

void OledDisplay::init() {
    // I2C OLED init hardened for real hardware bring-up:
    // configure pins first, enable pull-ups, then start the controller.
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_SDA);
    gpio_pull_up(PIN_SCL);
    i2c_init(I2C_PORT, I2C_BAUD);
    sleep_ms(20);
    const uint8_t init_cmds[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x1F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x02,
        0x81, 0x7F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF
    };
    send_command_list(init_cmds, sizeof(init_cmds));
    initialized_ = true;
    clear();
    update();
}

void OledDisplay::clear() { std::memset(buffer_, 0, sizeof(buffer_)); dirty_ = true; }

void OledDisplay::draw_pixel(int x, int y, bool on) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
    uint16_t idx = x + (y >> 3) * WIDTH;
    uint8_t mask = (1u << (y & 7));
    if (on) buffer_[idx] |= mask; else buffer_[idx] &= ~mask;
    dirty_ = true;
}

void OledDisplay::fill_rect(int x, int y, int w, int h, bool on) {
    for (int yy = y; yy < y + h; ++yy) for (int xx = x; xx < x + w; ++xx) draw_pixel(xx, yy, on);
}

void OledDisplay::draw_hline(int x, int y, int w, bool on) {
    for (int i = 0; i < w; ++i) draw_pixel(x + i, y, on);
}

void OledDisplay::draw_char_5x7(int x, int y, char c, bool invert) {
    const uint8_t* g = glyph_5x7(c);
    if (invert) fill_rect(x - 1, y - 1, 7, 9, true);
    for (int col = 0; col < 5; ++col) {
        const uint8_t bits = g[col];
        for (int row = 0; row < 7; ++row) {
            const bool on = ((bits >> row) & 1u) != 0;
            draw_pixel(x + col, y + row, invert ? !on : on);
        }
    }
}

void OledDisplay::draw_text(int x, int y, const char* text, bool invert) {
    for (int i = 0; text && text[i]; ++i) draw_char_5x7(x + i * 6, y, text[i], invert);
}

void OledDisplay::draw_text_small(int x, int y, const char* text, bool invert) {
    draw_text(x, y, text, invert);
}

void OledDisplay::draw_text_right(int right_x, int y, const char* text, bool invert) {
    if (!text) return;
    int len = 0;
    while (text[len]) ++len;
    draw_text(right_x - len * 6, y, text, invert);
}

void OledDisplay::update() {
    if (!initialized_ || !dirty_) return;
    send_command(0x21); send_command(0); send_command(WIDTH - 1);
    send_command(0x22); send_command(0); send_command(PAGES - 1);
    send_data(buffer_, sizeof(buffer_));
    dirty_ = false;
}
