#include "io/cap_pad_hybrid.h"
#include "hardware/pin_config.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include <cmath>

constexpr uint8_t CapPadHybrid::PAD_PINS[NUM_PADS];

float CapPadHybrid::clampf(float x, float lo, float hi) {
    return (x < lo) ? lo : (x > hi ? hi : x);
}

void CapPadHybrid::init(Config cfg) {
    cfg_ = cfg;

    gpio_init(PIN_PAD_ROW);
    gpio_set_dir(PIN_PAD_ROW, GPIO_OUT);
    gpio_put(PIN_PAD_ROW, 0);

    for (uint8_t i = 0; i < NUM_PADS; ++i) {
        gpio_init(PAD_PINS[i]);
        gpio_set_dir(PAD_PINS[i], GPIO_IN);
        gpio_disable_pulls(PAD_PINS[i]);
    }

    sleep_ms(40);
    calibrate();
}

uint32_t CapPadHybrid::measure_charge_us(uint8_t pad_pin) {
    gpio_put(PIN_PAD_ROW, 0);
    gpio_set_dir(pad_pin, GPIO_OUT);
    gpio_put(pad_pin, 0);
    sleep_us(cfg_.discharge_us);

    gpio_set_dir(pad_pin, GPIO_IN);
    gpio_disable_pulls(pad_pin);
    gpio_put(PIN_PAD_ROW, 1);

    uint32_t t = 0;
    while (!gpio_get(pad_pin) && t < cfg_.max_charge_us) {
        sleep_us(1);
        ++t;
    }

    gpio_put(PIN_PAD_ROW, 0);
    return t;
}

void CapPadHybrid::calibrate() {
    gpio_put(PIN_PAD_ROW, 0);
    sleep_ms(60);

    for (uint8_t i = 0; i < NUM_PADS; ++i) {
        uint64_t acc = 0;
        uint32_t samples[255] = {};
        const uint8_t n = cfg_.calib_samples;

        for (uint8_t s = 0; s < n; ++s) {
            const uint32_t t = measure_charge_us(PAD_PINS[i]);
            samples[s] = t;
            acc += t;
            sleep_us(100);
        }

        const float mean = static_cast<float>(acc) / static_cast<float>(n);
        float mad_acc = 0.0f;
        for (uint8_t s = 0; s < n; ++s) {
            mad_acc += std::fabs(static_cast<float>(samples[s]) - mean);
        }

        baseline_us_[i] = mean;
        noise_us_[i] = clampf(mad_acc / static_cast<float>(n), cfg_.min_noise_us, cfg_.max_noise_us);
        raw_us_[i] = static_cast<uint32_t>(baseline_us_[i]);
        pressure_[i] = 0.0f;
        integrator_[i] = 0;
    }

    state_ = 0;
    prev_state_ = 0;
    calibrated_ = true;
}

void CapPadHybrid::scan() {
    if (!calibrated_) return;

    prev_state_ = state_;
    uint16_t next_state = state_;

    for (uint8_t i = 0; i < NUM_PADS; ++i) {
        const uint32_t t = measure_charge_us(PAD_PINS[i]);
        raw_us_[i] = t;

        const float baseline = baseline_us_[i];
        const float delta = (t > baseline) ? (static_cast<float>(t) - baseline) : 0.0f;
        const float dynamic_thresh = cfg_.threshold_floor_us + noise_us_[i] * cfg_.threshold_noise_mul;
        const bool was_pressed = ((next_state >> i) & 1u) != 0;
        const float active_thresh = was_pressed ? (dynamic_thresh * cfg_.release_ratio) : dynamic_thresh;
        const bool touch_candidate = (delta > active_thresh) && (t < cfg_.max_charge_us);

        if (touch_candidate) {
            if (integrator_[i] < cfg_.integrator_max) ++integrator_[i];
        } else {
            if (integrator_[i] > 0) --integrator_[i];

            const float err = static_cast<float>(t) - baseline_us_[i];
            baseline_us_[i] += cfg_.baseline_alpha_idle * err;
            noise_us_[i] += cfg_.noise_alpha_idle * (std::fabs(err) - noise_us_[i]);
            noise_us_[i] = clampf(noise_us_[i], cfg_.min_noise_us, cfg_.max_noise_us);
        }

        if (!was_pressed && integrator_[i] >= cfg_.integrator_on) {
            next_state |= (1u << i);
        } else if (was_pressed && integrator_[i] <= cfg_.integrator_off) {
            next_state &= ~(1u << i);
        }

        float p = (delta - dynamic_thresh) / (220.0f + 2.0f * noise_us_[i]);
        p = clampf(p, 0.0f, 1.0f);
        p = std::pow(p, 1.45f);
        const float slew = (((next_state >> i) & 1u) != 0) ? 0.30f : 0.14f;
        pressure_[i] += slew * (p - pressure_[i]);
    }

    state_ = next_state;
}

bool CapPadHybrid::is_pressed(uint8_t pad) const {
    return (pad < NUM_PADS) ? (((state_ >> pad) & 1u) != 0) : false;
}

bool CapPadHybrid::just_pressed(uint8_t pad) const {
    if (pad >= NUM_PADS) return false;
    return (((state_ >> pad) & 1u) != 0) && (((prev_state_ >> pad) & 1u) == 0);
}

bool CapPadHybrid::just_released(uint8_t pad) const {
    if (pad >= NUM_PADS) return false;
    return (((state_ >> pad) & 1u) == 0) && (((prev_state_ >> pad) & 1u) != 0);
}

float CapPadHybrid::get_pressure(uint8_t pad) const {
    return (pad < NUM_PADS) ? pressure_[pad] : 0.0f;
}

uint32_t CapPadHybrid::get_raw_us(uint8_t pad) const {
    return (pad < NUM_PADS) ? raw_us_[pad] : 0u;
}

uint32_t CapPadHybrid::get_baseline_us(uint8_t pad) const {
    return (pad < NUM_PADS) ? static_cast<uint32_t>(baseline_us_[pad]) : 0u;
}

uint32_t CapPadHybrid::get_delta_us(uint8_t pad) const {
    if (pad >= NUM_PADS) return 0u;
    const float delta = static_cast<float>(raw_us_[pad]) - baseline_us_[pad];
    return (delta > 0.0f) ? static_cast<uint32_t>(delta) : 0u;
}

uint32_t CapPadHybrid::get_threshold_us(uint8_t pad) const {
    if (pad >= NUM_PADS) return 0u;
    return static_cast<uint32_t>(cfg_.threshold_floor_us + noise_us_[pad] * cfg_.threshold_noise_mul);
}
