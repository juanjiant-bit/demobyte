#include "pads.h"
#include "pico/stdlib.h"
#include <algorithm>

namespace io {
namespace {
constexpr uint ROW_PIN = 5;
constexpr uint COL_PINS[4] = {8, 9, 13, 14};
constexpr float TOUCH_ON_MUL   = 1.33f;
constexpr float TOUCH_HOLD_MUL = 1.18f;
constexpr float TOUCH_OFF_MUL  = 1.08f;
constexpr int   ON_CONFIRM     = 3;
constexpr int   OFF_CONFIRM    = 3;
constexpr int   COOLDOWN_TICKS = 10;
}

uint16_t Pads::read_pad(int idx) {
    const uint pin = COL_PINS[idx];
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 0);
    sleep_us(4);

    gpio_set_dir(ROW_PIN, GPIO_OUT);
    gpio_put(ROW_PIN, 1);

    gpio_set_dir(pin, GPIO_IN);
    uint16_t count = 0;
    while (!gpio_get(pin) && count < 1200) ++count;

    gpio_put(ROW_PIN, 0);
    return count;
}

void Pads::init() {
    gpio_init(ROW_PIN);
    gpio_set_dir(ROW_PIN, GPIO_OUT);
    gpio_put(ROW_PIN, 0);

    for (int i = 0; i < 4; ++i) {
        gpio_init(COL_PINS[i]);
        gpio_set_dir(COL_PINS[i], GPIO_IN);
    }

    for (int i = 0; i < 4; ++i) {
        uint32_t sum = 0;
        uint16_t lo = 65535, hi = 0;
        for (int n = 0; n < 48; ++n) {
            uint16_t v = read_pad(i);
            sum += v;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            sleep_us(300);
        }
        sum -= lo;
        sum -= hi;
        pads_[i].baseline = (uint16_t)(sum / 46u);
        pads_[i].raw = pads_[i].baseline;
    }
}

void Pads::update() {
    for (int i = 0; i < 4; ++i) {
        PadState& p = pads_[i];
        p.trigger = false;
        p.release = false;

        uint16_t v = read_pad(i);
        p.raw = v;
        if (p.cooldown > 0) p.cooldown--;

        if (!p.pressed) {
            p.baseline = (uint16_t)((p.baseline * 31u + v) / 32u);
        }

        const float on_th   = p.baseline * TOUCH_ON_MUL;
        const float hold_th = p.baseline * TOUCH_HOLD_MUL;
        const float off_th  = p.baseline * TOUCH_OFF_MUL;

        if (!p.pressed) {
            if (v > on_th) {
                if (p.on_count < 255) p.on_count++;
            } else {
                p.on_count = 0;
            }
            p.off_count = 0;
            if (p.cooldown == 0 && p.on_count >= ON_CONFIRM) {
                p.pressed = true;
                p.trigger = true;
                p.on_count = 0;
                p.cooldown = COOLDOWN_TICKS;
            }
        } else {
            if (v < off_th) {
                if (p.off_count < 255) p.off_count++;
            } else {
                p.off_count = 0;
            }
            if (p.off_count >= OFF_CONFIRM) {
                p.pressed = false;
                p.release = true;
                p.off_count = 0;
                p.pressure = 0.0f;
            }
        }

        if (p.pressed) {
            float num = float(int(v) - int(hold_th));
            float den = float(std::max(1, int(on_th - hold_th)));
            float target = num / den;
            if (target < 0.0f) target = 0.0f;
            if (target > 1.0f) target = 1.0f;
            p.pressure += 0.15f * (target - p.pressure);
        } else {
            p.pressure *= 0.8f;
        }
    }
}

} // namespace io
