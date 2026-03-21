#pragma once
// cap_pad_validate.h — BYT3 VALIDATE
// Manejo capacitivo para 1 fila / 4 columnas.
// Sin MUX. ROW=GP5, COL=GP8/9/13/14.
// Circuito: ROW --[1MΩ]-- PAD_COBRE -- COL
//           100nF cerámica ROW→GND
// COL pins sin pull interno — CRÍTICO.

#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include <cstdint>

class CapPadValidate {
public:
    static constexpr uint8_t NUM_PADS   = 4;
    static constexpr uint8_t ROW_PIN    = 5;
    static constexpr uint8_t COL_PINS[NUM_PADS] = {8, 9, 13, 14};

    // Preset de calibración
    struct Preset {
        uint32_t discharge_us  = 150;   // descarga antes de medir
        uint32_t max_charge_us = 2000;  // timeout = pad no conectado
        uint32_t hyst_on_us    = 30;    // delta mínimo para activar
        uint32_t hyst_off_us   = 18;    // delta mínimo para desactivar
        float    baseline_pct  = 0.25f; // threshold = baseline * (1 + pct)
        uint8_t  calib_samples = 200;   // muestras para baseline
    };

    void init(Preset p = Preset{}) {
        preset_ = p;
        // ROW: salida, empezar en LOW
        gpio_init(ROW_PIN);
        gpio_set_dir(ROW_PIN, GPIO_OUT);
        gpio_put(ROW_PIN, 0);
        // COLs: sin pull — lectura capacitiva
        for (uint8_t i = 0; i < NUM_PADS; i++) {
            gpio_init(COL_PINS[i]);
            gpio_set_dir(COL_PINS[i], GPIO_IN);
            gpio_disable_pulls(COL_PINS[i]);
        }
    }

    // Calibrar baseline — no tocar pads durante esto
    void calibrate() {
        for (uint8_t p = 0; p < NUM_PADS; p++) {
            uint64_t acc = 0;
            for (uint8_t s = 0; s < preset_.calib_samples; s++) {
                acc += measure_us(p);
            }
            baseline_[p] = (float)(acc / preset_.calib_samples);
            threshold_[p] = (uint32_t)(baseline_[p] * (1.0f + preset_.baseline_pct));
        }
        calibrated_ = true;
    }

    // Scan — llamar cada ~5ms desde Core1
    void scan() {
        uint16_t new_state = 0;
        for (uint8_t p = 0; p < NUM_PADS; p++) {
            uint32_t us = measure_us(p);
            raw_us_[p] = us;
            uint32_t b = (uint32_t)baseline_[p];
            uint32_t delta = (us > b) ? (us - b) : 0u;

            bool was_on = (state_ >> p) & 1;
            uint32_t thr = was_on ? threshold_[p] - preset_.hyst_off_us
                                  : threshold_[p];
            if (delta > thr) new_state |= (1u << p);
        }
        prev_state_ = state_;
        state_ = new_state;
    }

    bool     is_pressed   (uint8_t p) const { return (state_ >> p) & 1; }
    bool     just_pressed (uint8_t p) const { return  (state_ & ~prev_state_) >> p & 1; }
    bool     just_released(uint8_t p) const { return (~state_ &  prev_state_) >> p & 1; }
    uint16_t get_state()              const { return state_; }
    uint32_t get_raw_us   (uint8_t p) const { return raw_us_[p]; }
    uint32_t get_baseline (uint8_t p) const { return (uint32_t)baseline_[p]; }
    bool     is_calibrated()          const { return calibrated_; }

private:
    uint32_t measure_us(uint8_t pad_idx) {
        uint8_t col = COL_PINS[pad_idx];

        // Descarga: ROW LOW, COL OUTPUT LOW
        gpio_put(ROW_PIN, 0);
        gpio_set_dir(col, GPIO_OUT);
        gpio_put(col, 0);
        sleep_us(preset_.discharge_us);

        // Carga: ROW HIGH, COL INPUT sin pull
        gpio_set_dir(col, GPIO_IN);
        gpio_disable_pulls(col);
        gpio_put(ROW_PIN, 1);

        // Medir tiempo hasta que COL sube
        uint32_t t = 0;
        while (!gpio_get(col) && t < preset_.max_charge_us) {
            sleep_us(1);
            t++;
        }
        gpio_put(ROW_PIN, 0);
        return t;
    }

    Preset   preset_;
    float    baseline_[NUM_PADS]  = {};
    uint32_t threshold_[NUM_PADS] = {};
    uint32_t raw_us_[NUM_PADS]    = {};
    uint16_t state_               = 0;
    uint16_t prev_state_          = 0;
    bool     calibrated_          = false;
};
