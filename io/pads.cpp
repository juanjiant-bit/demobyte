#include "io/pads.h"
#include "hardware/adc.h"
#include <algorithm>
#include <cmath>

namespace controls {

namespace {

PadState g_pads[kNumPads];
PotState g_pots[kNumPots];

constexpr uint16_t kMaxCount = 2200;

// 🔥 NUEVOS THRESHOLDS (basados en tu hardware)
constexpr int kOnThreshold  = 60;
constexpr int kHoldThreshold = 45;

// 🔥 timing
constexpr uint32_t kHoldMs = 500;
constexpr uint32_t kRetrigLockMs = 100;

uint16_t read_cap_once(uint pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 0);
    sleep_us(4);

    gpio_set_dir(pin, GPIO_IN);
    gpio_disable_pulls(pin);

    uint16_t count = 0;
    while (!gpio_get(pin) && count < kMaxCount) ++count;
    return count;
}

uint16_t read_cap_avg(uint pin) {
    uint32_t sum = 0;
    for (int i = 0; i < 3; ++i) sum += read_cap_once(pin);
    return static_cast<uint16_t>(sum / 3);
}

uint16_t read_adc_avg(uint adc_input) {
    adc_select_input(adc_input);
    uint32_t sum = 0;
    for (int i = 0; i < 8; ++i) sum += adc_read();
    return static_cast<uint16_t>(sum / 8);
}

float smooth_pot(int idx, float target, float alpha) {
    g_pots[idx].value += alpha * (target - g_pots[idx].value);
    float diff = fabsf(g_pots[idx].value - g_pots[idx].stable);
    if (diff > 0.0025f) g_pots[idx].stable = g_pots[idx].value;
    return g_pots[idx].stable;
}

} // namespace

void init() {
    adc_init();
    for (int i = 0; i < kNumPots; ++i) {
        adc_gpio_init(kPotPins[i]);
    }
}

void update_1ms() {

    for (int i = 0; i < kNumPots; ++i) {
        const uint16_t raw = read_adc_avg(i);
        g_pots[i].raw = raw;
        float target = raw / 4095.0f;
        smooth_pot(i, target, i == 0 ? 0.06f : 0.1f);
    }

    uint32_t now = to_ms_since_boot(get_absolute_time());

    for (int i = 0; i < kNumPads; ++i) {
        auto& p = g_pads[i];

        p.trigger = false;
        p.release = false;

        uint16_t raw = read_cap_avg(kPadPins[i]);
        p.raw = raw;

        bool touching = raw >= kOnThreshold;
        bool holding  = raw >= kHoldThreshold;

        // 🔥 TRIGGER
        if (!p.pressed && touching) {
            if ((now - p.last_trigger_ms) > kRetrigLockMs) {
                p.pressed = true;
                p.trigger = true;
                p.held = false;
                p.touch_start_ms = now;
                p.last_trigger_ms = now;
            }
        }

        // 🔥 HOLD + AFTERTOUCH
        if (p.pressed && holding) {
            if ((now - p.touch_start_ms) > kHoldMs) {
                p.held = true;
            }

            float pr = (raw - kHoldThreshold) / 80.0f;
            pr = std::clamp(pr, 0.0f, 1.0f);
            p.pressure += 0.2f * (pr - p.pressure);
        }

        // 🔥 RELEASE (sin retrigger)
        if (p.pressed && !holding) {
            p.pressed = false;
            p.release = true;
            p.held = false;
            p.pressure = 0.0f;
        }
    }
}

const PadState& pad(int idx) { return g_pads[idx]; }

float volume() { return std::clamp(g_pots[0].stable, 0.0f, 1.0f); }
float morph()  { return std::clamp(g_pots[1].stable, 0.0f, 1.0f); }
float color()  { return std::clamp(g_pots[2].stable, 0.0f, 1.0f); }

}
