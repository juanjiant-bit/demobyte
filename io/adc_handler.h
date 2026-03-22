#pragma once

#include <cstdint>

class AdcHandler {
public:
    static constexpr uint8_t NUM_POTS = 7;
    static constexpr uint8_t DELAY_POT_CH = 7;
    static constexpr uint8_t CV_CH = 6;

    static constexpr uint8_t MUX_S0 = 2;
    static constexpr uint8_t MUX_S1 = 3;
    static constexpr uint8_t MUX_S2 = 4;
    static constexpr uint8_t ADC_PIN = 26; // GP26 = ADC0 / COM del 4051

    static constexpr float SMOOTH = 0.125f;
    static constexpr float CV_SMOOTH = 0.20f;
    static constexpr uint16_t HYST = 12;
    static constexpr uint8_t OVERSAMPLE = 4;

    void init();
    void poll();
    float get(uint8_t idx) const;
    float get_cv() const { return cv_smoothed_; }
    bool cv_active() const { return cv_smoothed_ > CV_NOISE_FLOOR; }

private:
    void select_channel(uint8_t ch);
    uint16_t read_adc();

    float smoothed_[NUM_POTS] = {};
    uint16_t last_raw_[NUM_POTS] = {};
    float cv_smoothed_ = 0.0f;
    static constexpr float CV_NOISE_FLOOR = 0.02f;
};
