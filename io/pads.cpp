
#include "io/pads.h"
#include "hardware/adc.h"
#include <algorithm>
#include <cmath>

namespace controls {
namespace {
PadState g_pads[kNumPads];
PotState g_pots[kNumPots];

// Lógica simple basada en RAW, no en ratios complejos.
// Ajustado a los valores reales que mostraste:
// baseline ~13..15 y touch ~18..22 => delta útil ~4..8 counts.
constexpr int kTouchDeltaOn = 4;      // trigger cuando raw sube ~4 counts sobre baseline
constexpr int kTouchDeltaHold = 2;    // mantener gate con menos delta
constexpr uint32_t kHoldMs = 500;     // desde acá pasa a hold/aftertouch

constexpr uint16_t kMaxCount = 2200;

uint16_t read_cap_once(uint pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 0);
    sleep_us(4);

    gpio_set_dir(pin, GPIO_IN);
    gpio_disable_pulls(pin);

    uint16_t count = 0;
    while (!gpio_get(pin) && count < kMaxCount) {
        ++count;
    }
    return count;
}

uint16_t read_cap_avg(uint pin) {
    uint32_t sum = 0;
    for (int i = 0; i < 3; ++i) {
        sum += read_cap_once(pin);
    }
    return static_cast<uint16_t>(sum / 3);
}

uint16_t trimmed_calibration(uint pin) {
    uint16_t vals[20];
    for (int i = 0; i < 20; ++i) {
        vals[i] = read_cap_avg(pin);
        sleep_us(180);
    }
    std::sort(vals, vals + 20);
    uint32_t sum = 0;
    for (int i = 4; i < 16; ++i) sum += vals[i];
    return static_cast<uint16_t>(sum / 12);
}

uint16_t read_adc_avg(uint adc_input) {
    adc_select_input(adc_input);
    uint32_t sum = 0;
    for (int i = 0; i < 8; ++i) {
        sum += adc_read();
    }
    return static_cast<uint16_t>(sum / 8);
}

float smooth_pot(int idx, float target, float alpha) {
    g_pots[idx].value += alpha * (target - g_pots[idx].value);
    float diff = fabsf(g_pots[idx].value - g_pots[idx].stable);
    if (diff > 0.0025f) {
        g_pots[idx].stable = g_pots[idx].value;
    }
    return g_pots[idx].stable;
}
}  // namespace

void init() {
    adc_init();
    for (int i = 0; i < kNumPots; ++i) {
        adc_gpio_init(kPotPins[i]);
    }

    for (int i = 0; i < kNumPads; ++i) {
        g_pads[i].baseline = trimmed_calibration(kPadPins[i]);
        g_pads[i].raw = g_pads[i].baseline;
    }

    for (int i = 0; i < kNumPots; ++i) {
        const uint16_t raw = read_adc_avg(i);
        g_pots[i].raw = raw;
        g_pots[i].value = raw / 4095.0f;
        g_pots[i].stable = g_pots[i].value;
    }
}

void update_1ms() {
    for (int i = 0; i < kNumPots; ++i) {
        const uint16_t raw = read_adc_avg(i);
        g_pots[i].raw = raw;
        float target = static_cast<float>(raw) / 4095.0f;
        if (i == 0) {
            smooth_pot(i, target, 0.06f);
        } else {
            smooth_pot(i, target, 0.10f);
        }
    }

    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    for (int i = 0; i < kNumPads; ++i) {
        auto& p = g_pads[i];
        p.trigger = false;
        p.release = false;

        const uint16_t raw = read_cap_avg(kPadPins[i]);
        p.raw = raw;

        // Baseline lento solo en reposo.
        if (!p.pressed) {
            p.baseline = static_cast<uint16_t>(0.999f * p.baseline + 0.001f * raw);
        }

        const int delta = int(raw) - int(p.baseline);

        if (!p.pressed) {
            if (delta >= kTouchDeltaOn) {
                p.pressed = true;
                p.trigger = true;
                p.release = false;
                p.held = false;
                p.touch_start_ms = now_ms;
                p.pressure = 0.0f;
            }
        } else {
            if (delta <= kTouchDeltaHold) {
                // soltar: no retrigger al release
                p.pressed = false;
                p.release = true;
                p.trigger = false;
                p.held = false;
                p.pressure = 0.0f;
            } else {
                if ((now_ms - p.touch_start_ms) >= kHoldMs) {
                    p.held = true;
                }

                // aftertouch expresivo mientras sigue tocado
                float pr = float(delta - kTouchDeltaHold) / 8.0f;
                pr = std::clamp(pr, 0.0f, 1.0f);
                p.pressure += 0.18f * (pr - p.pressure);
            }
        }
    }
}

const PadState& pad(int idx) {
    return g_pads[idx];
}

float volume() {
    float v = std::clamp(g_pots[0].stable, 0.0f, 1.0f);
    v = 0.03f + 0.97f * v;
    return v;
}

float morph() {
    return std::clamp(g_pots[1].stable, 0.0f, 1.0f);
}

float color() {
    return std::clamp(g_pots[2].stable, 0.0f, 1.0f);
}

}  // namespace controls
