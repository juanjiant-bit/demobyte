#include "synth/bytebeat_engine.h"
#include <algorithm>
#include <cmath>

namespace synth {
namespace {
static inline float norm8(uint32_t x) {
    const float v = static_cast<float>(x & 255u) * (1.0f / 127.5f) - 1.0f;
    return std::clamp(v, -1.0f, 1.0f);
}
}

void BytebeatEngine::init() {
    t_ = 0;
    formula_a_ = 0;
    formula_b_ = 1;
    morph_ = 0.0f;
    color_ = 0.0f;
    pressure_ = 0.0f;
    drone_on_ = false;
}

void BytebeatEngine::set_morph(float x) {
    morph_ = std::clamp(x, 0.0f, 1.0f);
}

void BytebeatEngine::set_color(float x) {
    color_ = std::clamp(x, 0.0f, 1.0f);
}

void BytebeatEngine::set_drone(bool on) {
    drone_on_ = on;
}

void BytebeatEngine::set_pressure(float x) {
    pressure_ = std::clamp(x, 0.0f, 1.0f);
}

void BytebeatEngine::next_formula_pair() {
    lfsr_ ^= lfsr_ << 13;
    lfsr_ ^= lfsr_ >> 17;
    lfsr_ ^= lfsr_ << 5;
    uint8_t next = static_cast<uint8_t>(lfsr_ % 6u);
    if (next == formula_a_) next = (next + 1u) % 6u;
    formula_a_ = next;
    formula_b_ = (formula_a_ + 1u + ((lfsr_ >> 8) % 3u)) % 6u;
    if (formula_b_ == formula_a_) formula_b_ = (formula_a_ + 1u) % 6u;
}

float BytebeatEngine::eval_formula(uint8_t id, uint32_t t) const {
    switch (id % 6u) {
        default:
        case 0: return norm8(((t >> 4) | (t >> 5) | (t * 3)));
        case 1: return norm8(((t * 5u & (t >> 7)) | (t * 3u & (t >> 10))));
        case 2: return norm8((t * (((t >> 11) & (t >> 8)) & 123u & (t >> 3))));
        case 3: return norm8((((t >> 3) * (t >> 5)) | (t >> 7)));
        case 4: return norm8(((t * 9u & (t >> 4)) | (t * 5u & (t >> 7)) | (t * 3u & (t >> 10))));
        case 5: return norm8(((t >> 4) ^ (t >> 7) ^ (t * 3u)));
    }
}

float BytebeatEngine::softclip(float x) const {
    return x / (1.0f + 0.75f * fabsf(x));
}

float BytebeatEngine::render() {
    // rate control: color moves from slower/rounder to brighter/faster
    const uint32_t base_step = drone_on_ ? 1u : 2u;
    const uint32_t color_step = static_cast<uint32_t>(color_ * 5.0f);
    t_ += base_step + color_step;

    const float a = eval_formula(formula_a_, t_);
    const float b = eval_formula(formula_b_, t_);
    float x = a + (b - a) * morph_;

    // tone tilt controlled by color
    const float prev = eval_formula(formula_a_, t_ - 1u);
    const float hp = x - 0.35f * prev;
    x = x * (1.0f - 0.65f * color_) + hp * (0.65f * color_);

    // pressure opens the drive slightly when holding pad 1
    float drive = 0.80f + 0.55f * color_ + 0.35f * pressure_;
    x *= drive;

    // keep the center of the sound more legible
    x *= 0.78f;
    return softclip(x);
}

}  // namespace synth
