#include "synth/bytebeat_engine.h"
#include <algorithm>

namespace synth {

void BytebeatEngine::init() {
    t_ = 0;
    formula_a_ = 0;
    formula_b_ = 1;
    drone_on_ = false;
}

void BytebeatEngine::set_macro(float m) {
    p_.macro = std::clamp(m, 0.0f, 1.0f);
    p_.morph = p_.macro;
    p_.tone = p_.macro;
    p_.drive = 0.15f + 0.85f * p_.macro;
}

void BytebeatEngine::randomize_formula() {
    formula_a_ = (formula_a_ + 3) & 3;
    formula_b_ = (formula_b_ + 1) & 3;
}

void BytebeatEngine::set_drone(bool on) {
    drone_on_ = on;
}

float BytebeatEngine::eval_formula(uint8_t id, uint32_t t) const {
    switch (id & 3) {
        default:
        case 0: return float(((t >> 5) | (t >> 8)) & (t >> 3)) / 127.0f * 2.0f - 1.0f;
        case 1: return float((t * ((t >> 11 & t >> 8) & 123 & t >> 3))) / 255.0f * 2.0f - 1.0f;
        case 2: return float(((t * 5 & t >> 7) | (t * 3 & t >> 10))) / 255.0f * 2.0f - 1.0f;
        case 3: return float(((t >> 4) ^ (t >> 7) ^ (t * 3))) / 255.0f * 2.0f - 1.0f;
    }
}

float BytebeatEngine::softclip(float x) const {
    return x / (1.0f + 0.8f * (x < 0 ? -x : x));
}

float BytebeatEngine::render() {
    uint32_t step = drone_on_ ? static_cast<uint32_t>(1 + p_.macro * 3.0f) : 1u;
    t_ += step;

    float a = eval_formula(formula_a_, t_);
    float b = eval_formula(formula_b_, t_);
    float x = a + (b - a) * p_.morph;

    float tone_hp = x - 0.25f * eval_formula(formula_a_, t_ - 1);
    x = x * (1.0f - 0.55f * p_.tone) + tone_hp * (0.55f * p_.tone);

    x *= (0.65f + 0.95f * p_.drive);
    return softclip(x);
}

}  // namespace synth
