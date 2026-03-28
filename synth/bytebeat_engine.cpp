
#include "synth/bytebeat_engine.h"
#include <algorithm>
#include <cmath>

namespace synth {
namespace {

static inline float clamp01(float x) {
    return std::clamp(x, 0.0f, 1.0f);
}

static inline float norm8(uint32_t x) {
    const float v = static_cast<float>(x & 255u) * (1.0f / 127.5f) - 1.0f;
    return std::clamp(v, -1.0f, 1.0f);
}

}  // namespace

void BytebeatEngine::init() {
    t_ = 0u;
    formula_a_ = 0u;
    formula_b_ = 1u;
    morph_ = 0.0f;
    color_ = 0.0f;
    pressure_ = 0.0f;
    drone_on_ = false;
    lfsr_ = 0x13579BDFu;
}

void BytebeatEngine::set_morph(float x) {
    morph_ = clamp01(x);
}

void BytebeatEngine::set_color(float x) {
    color_ = clamp01(x);
}

void BytebeatEngine::set_drone(bool on) {
    drone_on_ = on;
}

void BytebeatEngine::set_pressure(float x) {
    pressure_ = clamp01(x);
}

void BytebeatEngine::next_formula_pair() {
    lfsr_ ^= lfsr_ << 13;
    lfsr_ ^= lfsr_ >> 17;
    lfsr_ ^= lfsr_ << 5;

    uint8_t next = static_cast<uint8_t>(lfsr_ % 6u);
    if (next == formula_a_) next = static_cast<uint8_t>((next + 1u) % 6u);
    formula_a_ = next;

    formula_b_ = static_cast<uint8_t>((formula_a_ + 1u + ((lfsr_ >> 8) % 3u)) % 6u);
    if (formula_b_ == formula_a_) formula_b_ = static_cast<uint8_t>((formula_a_ + 1u) % 6u);
}

float BytebeatEngine::eval_formula(uint8_t id, uint32_t t) const {
    switch (id % 6u) {
        default:
        case 0: return norm8(((t >> 4) | (t >> 5) | (t * 3u)));
        case 1: return norm8(((t * 5u & (t >> 7)) | (t * 3u & (t >> 10))));
        case 2: return norm8((t * (((t >> 11) & (t >> 8)) & 123u & (t >> 3))));
        case 3: return norm8((((t >> 3) * (t >> 5)) | (t >> 7)));
        case 4: return norm8(((t * 9u & (t >> 4)) | (t * 5u & (t >> 7)) | (t * 3u & (t >> 10))));
        case 5: return norm8(((t >> 4) ^ (t >> 7) ^ (t * 3u)));
    }
}

float BytebeatEngine::softclip(float x) const {
    return x / (1.0f + 0.55f * fabsf(x));
}

float BytebeatEngine::render() {
    // Render limpio y compatible con tu header actual.
    // La idea es sacar del medio el ruido raro del procesamiento extra.
    const float rate_scale = 0.35f + color_ * 1.65f;      // ~0.35 .. 2.0
    const float pressure_slow = 1.0f - 0.45f * pressure_; // 1.0 .. 0.55
    const float drone_mul = drone_on_ ? 0.80f : 1.25f;

    float step_f = rate_scale * pressure_slow * drone_mul;

    uint32_t step = static_cast<uint32_t>(step_f);
    if (step < 1u) step = 1u;
    if (step > 4u) step = 4u;

    t_ += step;

    const float a = eval_formula(formula_a_, t_);
    const float b = eval_formula(formula_b_, t_);
    float x = a + (b - a) * morph_;

    // Sin diferenciador raro ni tilt agresivo.
    const float drive = 0.82f + 0.22f * color_ + 0.10f * pressure_;
    x *= drive;

    x *= 0.62f;
    return softclip(x);
}

}  // namespace synth
