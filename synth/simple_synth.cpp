#include "synth/simple_synth.h"
#include <cmath>

namespace {
constexpr float kTwoPi = 6.28318530718f;
constexpr float kNotes[4] = {130.81f, 98.0f, 164.81f, 220.0f};

inline float clampf(float x, float lo, float hi) {
    return (x < lo) ? lo : (x > hi ? hi : x);
}
}

void SimpleSynth::init(float sample_rate) {
    sample_rate_ = sample_rate;
}

void SimpleSynth::set_pot(float pot_01) {
    pot_ = clampf(pot_01, 0.0f, 1.0f);
}

void SimpleSynth::trigger_kick(float amount) {
    kick_env_ = 1.0f;
    kick_pitch_env_ = 1.0f;
    kick_phase_ = 0.0f;
    target_freq_hz_ = 50.0f + 70.0f * amount + 30.0f * pot_;
}

void SimpleSynth::trigger_snare(float amount) {
    snare_env_ = 0.65f + 0.35f * amount;
}

void SimpleSynth::trigger_hat(float amount) {
    hat_env_ = 0.45f + 0.55f * amount;
}

void SimpleSynth::trigger_snap(float amount) {
    snap_env_ = 0.6f + 0.4f * amount;
}

float SimpleSynth::noise_tick() {
    noise_state_ = noise_state_ * 1664525u + 1013904223u;
    const uint32_t bits = (noise_state_ >> 9) | 0x3f800000u;
    union { uint32_t u; float f; } v = { bits };
    return (v.f - 1.5f) * 2.0f;
}

void SimpleSynth::set_pad_state(bool snap, bool kick, bool snare, bool hat,
                                float snap_pressure, float kick_pressure,
                                float snare_pressure, float hat_pressure,
                                bool snap_rise, bool kick_rise, bool snare_rise, bool hat_rise) {
    const bool states[4] = {snap, kick, snare, hat};
    const float pressures[4] = {snap_pressure, kick_pressure, snare_pressure, hat_pressure};

    uint8_t mask = 0;
    int melodic_pad = -1;
    float melodic_pressure = 0.0f;

    for (int i = 0; i < 4; ++i) {
        if (states[i]) {
            mask |= (1u << i);
            melodic_pad = i;
            melodic_pressure = pressures[i];
        }
    }

    if (snap_rise) trigger_snap(snap_pressure);
    if (kick_rise) trigger_kick(kick_pressure);
    if (snare_rise) trigger_snare(snare_pressure);
    if (hat_rise) trigger_hat(hat_pressure);

    active_pad_mask_ = mask;

    if (melodic_pad >= 0) {
        const float accent = 1.0f + 0.06f * melodic_pressure + 0.28f * pot_;
        target_freq_hz_ = kNotes[melodic_pad] * accent;
    }
}

int16_t SimpleSynth::render_sample() {
    const bool gate = active_pad_mask_ != 0;

    const float attack = 0.004f + (1.0f - pot_) * 0.009f;
    const float release = 0.00035f + (pot_ * 0.0020f);
    env_ += gate ? attack * (1.0f - env_) : release * (0.0f - env_);
    env_ = clampf(env_, 0.0f, 1.0f);

    freq_hz_ += 0.0022f * (target_freq_hz_ - freq_hz_);
    phase_ += freq_hz_ / sample_rate_;
    if (phase_ >= 1.0f) phase_ -= 1.0f;

    const float sine = std::sin(kTwoPi * phase_);
    const float saw = 2.0f * phase_ - 1.0f;
    const float tri = 4.0f * std::fabs(phase_ - 0.5f) - 1.0f;
    const float shape = pot_;
    float tonal = ((1.0f - shape) * sine + shape * (0.65f * tri + 0.35f * saw));
    tonal *= env_;

    const float cutoff = 0.015f + 0.20f * pot_;
    filter_z_ += cutoff * (tonal - filter_z_);
    tonal = filter_z_;

    kick_pitch_env_ *= (0.9964f - 0.0008f * pot_);
    kick_env_ *= (0.9981f - 0.0007f * pot_);
    const float kick_freq = 38.0f + 120.0f * kick_pitch_env_ + 24.0f * pot_;
    kick_phase_ += kick_freq / sample_rate_;
    if (kick_phase_ >= 1.0f) kick_phase_ -= 1.0f;
    float kick = std::sin(kTwoPi * kick_phase_) * kick_env_;
    kick += 0.18f * noise_tick() * kick_env_ * kick_pitch_env_;

    snare_env_ *= (0.9925f - 0.0012f * pot_);
    snap_env_ *= (0.9860f - 0.0015f * pot_);
    float noise = noise_tick();
    float snap = noise * snap_env_;
    float snare = (0.78f * noise + 0.22f * std::sin(kTwoPi * phase_ * 1.9f)) * snare_env_;

    hat_env_ *= (0.985f - 0.0015f * pot_);
    const float bright_noise = noise_tick() - 0.82f * hat_filter_z_;
    hat_filter_z_ += 0.28f * (noise_tick() - hat_filter_z_);
    float hat = bright_noise * hat_env_ * (0.5f + 0.7f * pot_);

    float mix = 0.58f * tonal + 0.72f * kick + 0.38f * snare + 0.26f * hat + 0.18f * snap;
    mix = std::tanh((1.8f + 1.2f * pot_) * mix);

    const float out = clampf(mix * 26000.0f, -32767.0f, 32767.0f);
    return static_cast<int16_t>(out);
}
