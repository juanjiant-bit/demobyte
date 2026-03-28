
#include "synth/bytebeat_engine.h"
#include <algorithm>
#include <cmath>

namespace synth {
namespace {
static inline float clamp01(float x){
    return std::clamp(x, 0.0f, 1.0f);
}
static inline float lerp(float a, float b, float t){
    return a + (b - a) * t;
}
static inline float norm8(uint32_t x){
    const float v = static_cast<float>(x & 255u) * (1.0f / 127.5f) - 1.0f;
    return std::clamp(v, -1.0f, 1.0f);
}
static inline float tri_from_u32(uint32_t t, uint32_t div){
    float p = float(t % div) / float(div);
    return 2.0f * fabsf(2.0f * p - 1.0f) - 1.0f;
}
static inline float pulse_from_u32(uint32_t t, uint32_t div, float pw){
    float p = float(t % div) / float(div);
    return (p < pw) ? 1.0f : -1.0f;
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
    // Banco más seguro: menos fórmulas agresivas / más repetibles musicalmente.
    static const uint8_t safe_bank[4] = {0, 1, 3, 5};

    lfsr_ ^= lfsr_ << 13;
    lfsr_ ^= lfsr_ >> 17;
    lfsr_ ^= lfsr_ << 5;

    uint8_t next = safe_bank[lfsr_ & 3u];
    if (next == formula_a_) next = safe_bank[(lfsr_ >> 4) & 3u];
    if (next == formula_a_) next = safe_bank[((lfsr_ >> 8) & 3u)];
    formula_a_ = next;

    uint8_t nextb = safe_bank[(lfsr_ >> 10) & 3u];
    if (nextb == formula_a_) nextb = safe_bank[(nextb + 1u) & 3u];
    formula_b_ = nextb;
}

float BytebeatEngine::eval_formula(uint8_t id, uint32_t t) const {
    switch (id % 6u) {
        default:
        case 0: return norm8(((t >> 5) | (t >> 7) | (t * 3u)));
        case 1: return 0.72f * tri_from_u32(t, 1536u) + 0.14f * norm8(t >> 6);
        case 2: return 0.66f * pulse_from_u32(t, 2048u, 0.24f) + 0.16f * tri_from_u32(t, 896u);
        case 3: return norm8((((t >> 4) * (t >> 7)) | (t >> 8)));
        case 4: return norm8(((t * 5u & (t >> 7)) | (t * 3u & (t >> 10))));
        case 5: return norm8(((t >> 5) ^ (t >> 8) ^ (t * 3u)));
    }
}

float BytebeatEngine::softclip(float x) const {
    return x / (1.0f + 0.70f * fabsf(x));
}

float BytebeatEngine::render() {
    // Más grave por defecto para sacar el mosquito fijo.
    const uint32_t base_step = drone_on_ ? 1u : 1u;
    const uint32_t color_step = static_cast<uint32_t>(color_ * 3.0f);
    t_ += ((base_step + color_step) >> 1);

    const float a = eval_formula(formula_a_, t_);
    const float b = eval_formula(formula_b_, t_);
    float x = lerp(a, b, morph_);

    // Muchísimo menos tilt agudo.
    const float prev = eval_formula(formula_a_, (t_ > 0u) ? t_ - 1u : 0u);
    const float hp = x - 0.10f * prev;
    x = x * (1.0f - 0.18f * color_) + hp * (0.18f * color_);

    // Drive más suave.
    float drive = 0.78f + 0.28f * color_ + 0.16f * pressure_;
    x *= drive;

    // Low-pass muy simple para matar el pedal agudo constante.
    static float lp = 0.0f;
    lp += 0.07f * (x - lp);
    x = lp;

    x *= 0.82f;
    return softclip(x);
}

}  // namespace synth
