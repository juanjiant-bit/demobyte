#pragma once
#include <cstdint>

class SimpleSynth {
public:
    void init(float sample_rate);
    void set_pot(float pot_01);
    void set_pad_state(bool snap, bool kick, bool snare, bool hat,
                       float snap_pressure, float kick_pressure,
                       float snare_pressure, float hat_pressure,
                       bool snap_rise, bool kick_rise, bool snare_rise, bool hat_rise);
    int16_t render_sample();

private:
    void trigger_kick(float amount);
    void trigger_snare(float amount);
    void trigger_hat(float amount);
    void trigger_snap(float amount);
    float noise_tick();

    float sample_rate_ = 44100.0f;
    float phase_ = 0.0f;
    float freq_hz_ = 110.0f;
    float target_freq_hz_ = 110.0f;
    float env_ = 0.0f;
    float filter_z_ = 0.0f;
    float pot_ = 0.0f;
    uint8_t active_pad_mask_ = 0;

    float kick_env_ = 0.0f;
    float kick_phase_ = 0.0f;
    float kick_pitch_env_ = 0.0f;
    float snare_env_ = 0.0f;
    float snap_env_ = 0.0f;
    float hat_env_ = 0.0f;
    float hat_filter_z_ = 0.0f;
    uint32_t noise_state_ = 0x12345678u;
};
