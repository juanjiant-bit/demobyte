#include "io/pads.h"
#include "hardware/adc.h"
#include <algorithm>

namespace pads {
namespace {
PadState g_pads[kNumPads];
float g_macro = 0.0f;

constexpr float kTouchOnRatio = 1.34f;
constexpr float kTouchHoldRatio = 1.20f;
constexpr float kTouchOffRatio = 1.10f;
constexpr uint8_t kConfirmOn = 3;
constexpr uint8_t kConfirmOff = 3;
constexpr uint16_t kCooldownMs = 55;
constexpr uint16_t kMaxCount = 2000;

uint16_t read_cap(uint pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 0);
    sleep_us(3);

    gpio_set_dir(pin, GPIO_IN);
    gpio_disable_pulls(pin);

    uint16_t count = 0;
    while (!gpio_get(pin) && count < kMaxCount) {
        ++count;
    }
    return count;
}

uint16_t trimmed_calibration(uint pin) {
    uint16_t vals[16];
    for (int i = 0; i < 16; ++i) {
        vals[i] = read_cap(pin);
        sleep_us(200);
    }
    std::sort(vals, vals + 16);
    uint32_t sum = 0;
    for (int i = 3; i < 13; ++i) sum += vals[i];
    return static_cast<uint16_t>(sum / 10);
}
}

void init() {
    adc_init();
    adc_gpio_init(kPotPin);

    for (int i = 0; i < kNumPads; ++i) {
        g_pads[i].baseline = trimmed_calibration(kSensePins[i]);
        g_pads[i].raw = g_pads[i].baseline;
    }
}

void update_1ms() {
    adc_select_input(0);
    uint16_t pot = adc_read();
    float m = static_cast<float>(pot) / 4095.0f;
    g_macro += 0.08f * (m - g_macro);

    for (int i = 0; i < kNumPads; ++i) {
        auto& p = g_pads[i];
        p.trigger = false;
        p.release = false;
        if (p.cooldown_ms > 0) --p.cooldown_ms;

        uint16_t raw = read_cap(kSensePins[i]);
        p.raw = raw;

        if (!p.pressed) {
            p.baseline = static_cast<uint16_t>(0.995f * p.baseline + 0.005f * raw);
        }

        float on_th = p.baseline * kTouchOnRatio;
        float hold_th = p.baseline * kTouchHoldRatio;
        float off_th = p.baseline * kTouchOffRatio;

        bool touch_high = raw > on_th;
        bool touch_hold = raw > hold_th;
        bool touch_low = raw < off_th;

        if (!p.pressed) {
            if (touch_high && p.cooldown_ms == 0) {
                if (p.on_count < 255) ++p.on_count;
            } else if (p.on_count > 0) {
                --p.on_count;
            }

            if (p.on_count >= kConfirmOn) {
                p.pressed = true;
                p.trigger = true;
                p.on_count = 0;
                p.off_count = 0;
                p.cooldown_ms = kCooldownMs;
            }
        } else {
            if (touch_low) {
                if (p.off_count < 255) ++p.off_count;
            } else {
                p.off_count = 0;
            }

            if (p.off_count >= kConfirmOff) {
                p.pressed = false;
                p.release = true;
                p.off_count = 0;
                p.pressure = 0.0f;
            } else {
                float span = std::max(1.0f, on_th - hold_th);
                float pr = (static_cast<float>(raw) - hold_th) / (on_th + span);
                pr = std::clamp(pr * 2.2f, 0.0f, 1.0f);
                p.pressure += 0.2f * (pr - p.pressure);
            }
        }
    }
}

const PadState& get(int idx) {
    return g_pads[idx];
}

float macro() {
    return g_macro;
}

}  // namespace pads
