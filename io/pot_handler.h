#pragma once
// pot_handler.h — BYT3 VALIDATE
// Lectura directa de un único pote en GP26 (ADC0), sin CD4051.
// Smoothing EMA + histéresis para eliminar jitter.

#include "hardware/adc.h"
#include "hardware/gpio.h"
#include <cstdint>

class PotHandler {
public:
    static constexpr uint8_t  ADC_INPUT  = 0;      // ADC0 = GP26
    static constexpr uint8_t  ADC_GPIO   = 26;
    static constexpr float    SMOOTH     = 0.08f;  // EMA suave
    static constexpr uint16_t HYST       = 10;     // deadband 12-bit

    void init() {
        adc_init();
        adc_gpio_init(ADC_GPIO);
        adc_select_input(ADC_INPUT);
        // Warm-up: leer y fijar valor inicial
        uint32_t acc = 0;
        for (int i = 0; i < 8; i++) acc += adc_read();
        last_raw_ = (uint16_t)(acc / 8);
        smoothed_ = last_raw_ / 4095.0f;
    }

    // Llamar desde Core1 cada ~5ms
    void poll() {
        adc_select_input(ADC_INPUT);
        uint16_t raw = adc_read();
        int32_t delta = (int32_t)raw - (int32_t)last_raw_;
        if (delta < 0) delta = -delta;
        if (delta >= (int32_t)HYST) last_raw_ = raw;
        else raw = last_raw_;
        smoothed_ += SMOOTH * (raw / 4095.0f - smoothed_);
    }

    // Valor normalizado 0.0–1.0
    float get() const { return smoothed_; }

private:
    uint16_t last_raw_ = 0;
    float    smoothed_ = 0.0f;
};
