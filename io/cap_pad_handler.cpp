#include "cap_pad_handler.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "pico/stdlib.h"
#include <cmath>

constexpr uint8_t CapPadHandler::ROW_PINS[];
constexpr uint8_t CapPadHandler::COL_PINS[];

void CapPadHandler::init(Preset p) {
    preset_ = p;
    for (uint8_t r = 0; r < ROWS; ++r) {
        gpio_init(ROW_PINS[r]);
        gpio_set_dir(ROW_PINS[r], GPIO_OUT);
        gpio_put(ROW_PINS[r], 0);
    }
    for (uint8_t c = 0; c < COLS; ++c) {
        gpio_init(COL_PINS[c]);
        gpio_set_dir(COL_PINS[c], GPIO_IN);
        gpio_disable_pulls(COL_PINS[c]);
    }
    sleep_ms(20);
}

uint32_t CapPadHandler::measure_charge_us(uint8_t row, uint8_t col) {
    const uint8_t rp = ROW_PINS[row];
    const uint8_t cp = COL_PINS[col];

    gpio_put(rp, 0);
    sleep_us(preset_.discharge_us);

    const uint32_t t0 = time_us_32();
    gpio_put(rp, 1);
    while (!gpio_get(cp)) {
        if ((time_us_32() - t0) >= preset_.max_charge_us) {
            gpio_put(rp, 0);
            return preset_.max_charge_us;
        }
    }
    const uint32_t dt = time_us_32() - t0;
    gpio_put(rp, 0);
    return dt;
}

void CapPadHandler::calibrate() {
    for (uint8_t r = 0; r < ROWS; ++r) gpio_put(ROW_PINS[r], 0);
    sleep_ms(30);

    for (uint8_t r = 0; r < ROWS; ++r) {
        for (uint8_t c = 0; c < COLS; ++c) {
            const uint8_t idx = (uint8_t)(r * COLS + c);
            uint64_t sum = 0;
            for (uint8_t s = 0; s < preset_.calib_samples; ++s) {
                sum += measure_charge_us(r, c);
                sleep_us(80);
            }
            const float baseline = (float)(sum / preset_.calib_samples);
            baseline_f_[idx] = baseline;
            raw_us_[idx] = (uint32_t)baseline;
            pressure_smooth_[idx] = 0.0f;
            threshold_us_[idx] = (uint32_t)(baseline * 1.25f);
        }
    }

    state_confirmed_ = 0;
    state_prev_ = 0;
    calibrated_ = true;
}

float CapPadHandler::compute_pressure(uint32_t delta_us) const {
    if (delta_us < preset_.hyst_on_us) return 0.0f;
    float n = (float)(delta_us - preset_.hyst_on_us) / (float)preset_.at_range_us;
    if (n < 0.0f) n = 0.0f;
    if (n > 1.0f) n = 1.0f;
    return powf(n, preset_.at_curve);
}

void CapPadHandler::scan() {
    if (!calibrated_) return;

    state_prev_ = state_confirmed_;
    uint16_t new_state = 0;

    for (uint8_t r = 0; r < ROWS; ++r) {
        for (uint8_t c = 0; c < COLS; ++c) {
            const uint8_t idx = (uint8_t)(r * COLS + c);
            const uint32_t t = measure_charge_us(r, c);
            raw_us_[idx] = t;

            const uint32_t b = (uint32_t)baseline_f_[idx];
            const uint32_t d = (t > b) ? (t - b) : 0u;
            const bool was_on = ((state_confirmed_ >> idx) & 1u) != 0u;
            const bool touched = was_on ? (d >= preset_.hyst_off_us)
                                        : (t >= threshold_us_[idx] && t < preset_.max_charge_us);

            if (touched) {
                new_state |= (uint16_t)(1u << idx);
            }

            const float raw_p = touched ? compute_pressure(d) : 0.0f;
            const float alpha = (raw_p > pressure_smooth_[idx]) ? 0.4f : 0.15f;
            pressure_smooth_[idx] += alpha * (raw_p - pressure_smooth_[idx]);

            if (!touched) {
                baseline_f_[idx] += preset_.baseline_alpha * ((float)t - baseline_f_[idx]);
                threshold_us_[idx] = (uint32_t)(baseline_f_[idx] * 1.25f);
            }
        }
    }

    state_confirmed_ = new_state;
}

bool CapPadHandler::is_pressed(uint8_t pad) const {
    if (pad >= NUM_PADS) return false;
    return ((state_confirmed_ >> pad) & 1u) != 0u;
}

bool CapPadHandler::just_pressed(uint8_t pad) const {
    if (pad >= NUM_PADS) return false;
    return (((state_confirmed_ >> pad) & 1u) != 0u) && (((state_prev_ >> pad) & 1u) == 0u);
}

bool CapPadHandler::just_released(uint8_t pad) const {
    if (pad >= NUM_PADS) return false;
    return (((state_confirmed_ >> pad) & 1u) == 0u) && (((state_prev_ >> pad) & 1u) != 0u);
}

float CapPadHandler::get_pressure(uint8_t pad) const {
    if (pad >= NUM_PADS) return 0.0f;
    return pressure_smooth_[pad];
}
