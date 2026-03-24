#pragma once

#include <cstdint>

class CapPadHandler {
public:
    static constexpr uint8_t ROWS = 3;
    static constexpr uint8_t COLS = 5;
    static constexpr uint8_t NUM_PADS = ROWS * COLS;

    static constexpr uint8_t ROW_PINS[ROWS] = {5, 6, 7};
    static constexpr uint8_t COL_PINS[COLS] = {8, 9, 13, 14, 15};

    struct Preset {
        uint32_t discharge_us;
        uint32_t max_charge_us;
        uint32_t hyst_on_us;
        uint32_t hyst_off_us;
        uint32_t at_range_us;
        float at_curve;
        uint8_t calib_samples;
        float baseline_alpha;

        static constexpr Preset DRY() {
            return {150, 2000, 30, 18, 120, 1.5f, 120, 0.002f};
        }
    };

    void init(Preset p = Preset::DRY());
    void calibrate();
    void scan();

    bool is_pressed(uint8_t pad) const;
    bool just_pressed(uint8_t pad) const;
    bool just_released(uint8_t pad) const;
    uint16_t get_state()               const { return state_confirmed_; }
    uint32_t get_raw_us(uint8_t pad)   const { return (pad < NUM_PADS) ? raw_us_[pad] : 0u; }
    uint32_t get_baseline_us(uint8_t p) const { return (p < NUM_PADS) ? (uint32_t)baseline_f_[p] : 0u; }
    float get_pressure(uint8_t pad) const;

private:
    uint32_t measure_charge_us(uint8_t row, uint8_t col);
    float compute_pressure(uint32_t delta_us) const;

    Preset preset_;
    float baseline_f_[NUM_PADS] = {};
    uint32_t threshold_us_[NUM_PADS] = {};
    uint32_t raw_us_[NUM_PADS] = {};
    float pressure_smooth_[NUM_PADS] = {};
    uint16_t state_confirmed_ = 0;
    uint16_t state_prev_ = 0;
    bool calibrated_ = false;
};
