#pragma once
#include <cstdint>

class CapPadHybrid {
public:
    enum Pad : uint8_t {
        SNAP = 0,
        KICK = 1,
        SNARE = 2,
        HAT = 3,
        NUM_PADS = 4,
    };

    struct Config {
        uint32_t discharge_us = 150;
        uint32_t max_charge_us = 2500;
        uint8_t calib_samples = 96;
        float baseline_alpha_idle = 0.0040f;
        float noise_alpha_idle = 0.050f;
        float min_noise_us = 3.0f;
        float max_noise_us = 90.0f;
        float threshold_floor_us = 14.0f;
        float threshold_noise_mul = 5.5f;
        float release_ratio = 0.50f;
        uint8_t integrator_on = 4;
        uint8_t integrator_off = 1;
        uint8_t integrator_max = 7;
    };

    void init(Config cfg = Config{});
    void calibrate();
    void scan();

    bool is_pressed(uint8_t pad) const;
    bool just_pressed(uint8_t pad) const;
    bool just_released(uint8_t pad) const;
    float get_pressure(uint8_t pad) const;

    uint32_t get_raw_us(uint8_t pad) const;
    uint32_t get_baseline_us(uint8_t pad) const;
    uint32_t get_delta_us(uint8_t pad) const;
    uint32_t get_threshold_us(uint8_t pad) const;
    bool is_calibrated() const { return calibrated_; }

private:
    uint32_t measure_charge_us(uint8_t pad_pin);
    static float clampf(float x, float lo, float hi);

    Config cfg_{};
    static constexpr uint8_t PAD_PINS[NUM_PADS] = {8, 9, 13, 14};

    float baseline_us_[NUM_PADS] = {};
    float noise_us_[NUM_PADS] = {};
    uint32_t raw_us_[NUM_PADS] = {};
    float pressure_[NUM_PADS] = {};
    uint8_t integrator_[NUM_PADS] = {};
    uint16_t state_ = 0;
    uint16_t prev_state_ = 0;
    bool calibrated_ = false;
};
