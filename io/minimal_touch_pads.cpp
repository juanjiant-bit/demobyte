#include "minimal_touch_pads.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "pico/stdlib.h"

constexpr uint8_t MinimalTouchPads::PAD_PINS[];

void MinimalTouchPads::init(Preset p) {
    preset_ = p;
    gpio_init(DRIVE_PIN);
    gpio_set_dir(DRIVE_PIN, GPIO_OUT);
    gpio_put(DRIVE_PIN, 0);

    for (uint8_t i = 0; i < NUM_PADS; ++i) {
        gpio_init(PAD_PINS[i]);
        gpio_set_dir(PAD_PINS[i], GPIO_IN);
        gpio_disable_pulls(PAD_PINS[i]);
    }
    sleep_ms(20);
    calibrate();
}

uint32_t MinimalTouchPads::measure_charge_us(uint8_t pin) {
    gpio_put(DRIVE_PIN, 0);
    sleep_us(preset_.discharge_us);
    const uint32_t t0 = time_us_32();
    gpio_put(DRIVE_PIN, 1);
    while (!gpio_get(pin)) {
        if ((time_us_32() - t0) >= preset_.max_charge_us) {
            gpio_put(DRIVE_PIN, 0);
            return preset_.max_charge_us;
        }
    }
    const uint32_t dt = time_us_32() - t0;
    gpio_put(DRIVE_PIN, 0);
    return dt;
}

void MinimalTouchPads::calibrate() {
    gpio_put(DRIVE_PIN, 0);
    sleep_ms(30);
    for (uint8_t i = 0; i < NUM_PADS; ++i) {
        uint64_t sum = 0;
        for (uint8_t s = 0; s < preset_.calib_samples; ++s) {
            sum += measure_charge_us(PAD_PINS[i]);
            sleep_us(100);
        }
        baseline_f_[i] = float(sum / preset_.calib_samples);
        raw_us_[i] = (uint32_t)baseline_f_[i];
    }
    state_confirmed_ = 0;
    state_prev_ = 0;
}

void MinimalTouchPads::scan() {
    state_prev_ = state_confirmed_;
    uint16_t next_state = 0;

    for (uint8_t i = 0; i < NUM_PADS; ++i) {
        const uint32_t raw = measure_charge_us(PAD_PINS[i]);
        raw_us_[i] = raw;
        const uint32_t base = (uint32_t)baseline_f_[i];
        const uint32_t delta = (raw > base) ? (raw - base) : 0u;
        const bool was_on = ((state_confirmed_ >> i) & 1u) != 0;
        // Threshold relativo cuando hyst_on_us==0: usa 15%/8% del baseline.
        // Esto funciona para pads grandes donde el baseline varía por pad.
        const uint32_t hyst_on  = (preset_.hyst_on_us > 0)
                                  ? preset_.hyst_on_us
                                  : (uint32_t)(baseline_f_[i] * 0.15f);
        const uint32_t hyst_off = (preset_.hyst_off_us > 0)
                                  ? preset_.hyst_off_us
                                  : (uint32_t)(baseline_f_[i] * 0.08f);
        const bool is_on = was_on ? (delta >= hyst_off)
                                  : (delta >= hyst_on && raw < preset_.max_charge_us);
        if (is_on) next_state |= (1u << i);
        if (!is_on) {
            baseline_f_[i] += preset_.baseline_alpha * (float(raw) - baseline_f_[i]);
        }
    }

    state_confirmed_ = next_state;
}

bool MinimalTouchPads::is_pressed(uint8_t pad) const {
    return (pad < NUM_PADS) ? (((state_confirmed_ >> pad) & 1u) != 0) : false;
}

bool MinimalTouchPads::just_pressed(uint8_t pad) const {
    if (pad >= NUM_PADS) return false;
    return (((state_confirmed_ >> pad) & 1u) != 0) && (((state_prev_ >> pad) & 1u) == 0);
}

bool MinimalTouchPads::just_released(uint8_t pad) const {
    if (pad >= NUM_PADS) return false;
    return (((state_confirmed_ >> pad) & 1u) == 0) && (((state_prev_ >> pad) & 1u) != 0);
}

uint32_t MinimalTouchPads::get_raw_us(uint8_t pad) const {
    return (pad < NUM_PADS) ? raw_us_[pad] : 0u;
}

uint32_t MinimalTouchPads::get_baseline_us(uint8_t pad) const {
    return (pad < NUM_PADS) ? (uint32_t)baseline_f_[pad] : 0u;
}
