// adc_handler.cpp — Bytebeat Machine V1.21
// CAMBIOS: NUM_POTS=7. Pots 0-5 → CH0-CH5. Pot 6 (delay) → CH7 (leído aparte).
// CV sigue en CH6 sin cambios.
#include "adc_handler.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"

void AdcHandler::init() {
    gpio_init(MUX_S0); gpio_set_dir(MUX_S0, GPIO_OUT);
    gpio_init(MUX_S1); gpio_set_dir(MUX_S1, GPIO_OUT);
    gpio_init(MUX_S2); gpio_set_dir(MUX_S2, GPIO_OUT);
    adc_init();
    adc_gpio_init(ADC_PIN);
    adc_select_input(0);
}

void AdcHandler::select_channel(uint8_t ch) {
    gpio_put(MUX_S0,  ch & 0x01);
    gpio_put(MUX_S1, (ch >> 1) & 0x01);
    gpio_put(MUX_S2, (ch >> 2) & 0x01);
}

uint16_t AdcHandler::read_adc() { return adc_read(); }

void AdcHandler::poll() {
    auto read_mux_avg = [this](uint8_t ch) -> uint16_t {
        select_channel(ch);
        sleep_us(8);
        (void)read_adc(); // dummy read after switching the 4051
        uint32_t acc = 0;
        for (uint8_t n = 0; n < OVERSAMPLE; ++n) acc += read_adc();
        return (uint16_t)(acc / OVERSAMPLE);
    };

    // Pots 0-5: CH0-CH5 del CD4051
    for (uint8_t i = 0; i < NUM_POTS - 1; i++) {  // 0..5
        uint16_t raw = read_mux_avg(i);
        int32_t delta = (int32_t)raw - (int32_t)last_raw_[i];
        if (delta < 0) delta = -delta;
        if (delta < (int32_t)HYST) raw = last_raw_[i];
        else                       last_raw_[i] = raw;

        float target  = raw / 4095.0f;
        smoothed_[i] += SMOOTH * (target - smoothed_[i]);
    }
    // Pot 6 (delay): CH7 del CD4051 (S2=1,S1=1,S0=1)
    {
        uint16_t raw = read_mux_avg(DELAY_POT_CH);
        int32_t delta = (int32_t)raw - (int32_t)last_raw_[6];
        if (delta < 0) delta = -delta;
        if (delta < (int32_t)HYST) raw = last_raw_[6];
        else                       last_raw_[6] = raw;
        smoothed_[6] += SMOOTH * ((raw / 4095.0f) - smoothed_[6]);
    }

    // ── CV IN: CH6 del MUX ───────────────────────────────────────
    float cv_raw  = read_mux_avg(CV_CH) / 4095.0f;
    if (cv_raw < 0.0f) cv_raw = 0.0f;
    if (cv_raw > 1.0f) cv_raw = 1.0f;
    cv_smoothed_ += CV_SMOOTH * (cv_raw - cv_smoothed_);
}

float AdcHandler::get(uint8_t idx) {
    if (idx >= NUM_POTS) return 0.0f;
    return smoothed_[idx];
}
