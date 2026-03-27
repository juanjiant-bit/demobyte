
#include "io/pads.h"
#include "hardware/adc.h"
#include <algorithm>
#include <cmath>

namespace controls {
namespace {
PadState g_pads[kNumPads];
PotState g_pots[kNumPots];

// Esta versión prioriza recuperar triggers reales sin perder aftertouch.
// Menos "latch" y más edge detection con histéresis simple.
constexpr float kTouchOnRatio   = 1.16f;
constexpr float kTouchHoldRatio = 1.10f;
constexpr float kTouchOffRatio  = 1.05f;

constexpr uint8_t kConfirmOn  = 2;
constexpr uint8_t kConfirmOff = 1;

constexpr uint16_t kCooldownMs = 8;
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

    for (int i = 0; i < kNumPads; ++i) {
        auto& p = g_pads[i];
        const bool was_pressed = p.pressed;

        p.trigger = false;
        p.release = false;
        if (p.cooldown_ms > 0) --p.cooldown_ms;

        const uint16_t raw = read_cap_avg(kPadPins[i]);
        p.raw = raw;

        // baseline solo en reposo, más lento para no perseguir al dedo
        if (!was_pressed) {
            p.baseline = static_cast<uint16_t>(0.999f * p.baseline + 0.001f * raw);
        }

        const float on_th   = p.baseline * kTouchOnRatio;
        const float hold_th = p.baseline * kTouchHoldRatio;
        const float off_th  = p.baseline * kTouchOffRatio;

        const bool touch_on  = raw > on_th;
        const bool touch_off = raw < off_th;

        if (!was_pressed) {
            if (touch_on && p.cooldown_ms == 0) {
                if (p.on_count < 255) ++p.on_count;
            } else {
                p.on_count = 0;
            }

            if (p.on_count >= kConfirmOn) {
                p.pressed = true;
                p.trigger = true;
                p.release = false;
                p.on_count = 0;
                p.off_count = 0;
                p.cooldown_ms = kCooldownMs;
            } else {
                p.pressed = false;
            }
        } else {
            if (touch_off) {
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
                p.pressed = true;
            }
        }

        // presión expresiva independiente del trigger
        if (p.pressed) {
            const float span = std::max(1.0f, on_th - hold_th);
            float pr = (static_cast<float>(raw) - hold_th) / span;
            pr = std::clamp(pr, 0.0f, 1.0f);
            p.pressure += 0.18f * (pr - p.pressure);
        } else {
            p.pressure *= 0.65f;
        }

        // seguridad: si por alguna razón quedó presionado demasiado estable,
        // el release vuelve a abrir la puerta a un nuevo trigger rápido.
        if (!p.pressed && !was_pressed) {
            p.on_count = 0;
            p.off_count = 0;
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
