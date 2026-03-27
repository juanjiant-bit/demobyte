
#include "synth/bytebeat_engine.h"
#include "pico/stdlib.h"
#include <algorithm>
#include <cmath>

namespace synth {
namespace {
static inline float norm8(uint32_t x) {
    const float v = static_cast<float>(x & 255u) * (1.0f / 127.5f) - 1.0f;
    return std::clamp(v, -1.0f, 1.0f);
}

static inline float clamp01(float x) {
    return std::clamp(x, 0.0f, 1.0f);
}
}

void BytebeatEngine::init() {
    t_ = 0;
    formula_a_ = 0;
    formula_b_ = 1;
    morph_ = 0.0f;
    color_ = 0.0f;
    mod_ = 0.0f;
    pressure_ = 0.0f;
    drone_on_ = false;
}

void BytebeatEngine::set_morph(float x) {
    morph_ = clamp01(x);
}

void BytebeatEngine::set_color(float x) {
    color_ = clamp01(x);
}

void BytebeatEngine::set_mod(float x) {
    mod_ = clamp01(x);
}

void BytebeatEngine::set_drone(bool on) {
    drone_on_ = on;
}

void BytebeatEngine::set_pressure(float x) {
    pressure_ = clamp01(x);
}

void BytebeatEngine::randomize_on_boot() {
    // Mezcla un poco de timing real del arranque para no quedar siempre igual.
    uint32_t s = time_us_32();
    s ^= (s << 7);
    s ^= (s >> 9);
    s ^= 0xA53C9E17u;
    lfsr_ ^= s;
    next_formula_pair();
    next_formula_pair();
    next_formula_pair();
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
    // Aftertouch (color_) ahora manda el rate/tape.
    // Más presión en pad1 => color_ más bajo => más lento/oscuro.
    const uint32_t base_step = drone_on_ ? 1u : 2u;
    const uint32_t color_step = static_cast<uint32_t>(color_ * 7.0f);
    t_ += base_step + color_step;

    const float a = eval_formula(formula_a_, t_);
    const float b = eval_formula(formula_b_, t_);
    float x = a + (b - a) * morph_;

    // color_ (aftertouch) sigue siendo el gesto vivo de brillo/rate
    const float prev = eval_formula(formula_a_, t_ - 1u);
    const float hp = x - 0.35f * prev;
    x = x * (1.0f - 0.65f * color_) + hp * (0.65f * color_);

    // mod_ (antes pote color) ahora controla cuerpo + complejidad
    const float complexity = 0.75f + 1.85f * mod_;
    x *= complexity;

    // cuerpo low-mid extra con el pote mod
    const float body = sinf(0.00011f * float(t_)) * (0.10f + 0.28f * mod_);
    x += body;

    // pressure sigue abriendo drive
    float drive = 0.80f + 0.35f * color_ + 0.30f * pressure_ + 0.25f * mod_;
    x *= drive;

    x *= 0.72f;
    return softclip(x);
}

}  // namespace synth
