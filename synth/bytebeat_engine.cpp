
#include "bytebeat_engine.h"
#include <cmath>

namespace synth {
namespace {
static inline float sat(float x) {
    return x / (1.0f + fabsf(x));
}

static inline float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}
}

void BytebeatEngine::init() {
    t_ = 0;
    rand_ = 0xC0FFEE11u;
    formula_a_ = 0;
    formula_b_ = 5;
}

void BytebeatEngine::next_formula_pair() {
    rand_ ^= rand_ << 13;
    rand_ ^= rand_ >> 17;
    rand_ ^= rand_ << 5;

    int next_a = int(rand_ % 10u);
    int next_b = int((rand_ >> 8) % 10u);

    if (next_a == formula_a_) next_a = (next_a + 1) % 10;
    if (next_b == next_a) next_b = (next_b + 3) % 10;

    formula_a_ = next_a;
    formula_b_ = next_b;
}

float BytebeatEngine::render_formula(int id, uint32_t tt, float c) const {
    const float t = float(tt);

    // c ya viene expandido desde render()
    const float s1 = sinf(t * (0.00020f + c * 0.00018f));
    const float s2 = sinf(t * (0.00009f + c * 0.00014f));
    const float s3 = sinf(t * (0.00003f + c * 0.00008f));
    const float tri = 2.0f * fabsf(fmodf(t * (0.00007f + c * 0.00010f), 1.0f) - 0.5f) - 1.0f;

    switch (id) {
        default:
        case 0: return sat((((tt >> 5) | (tt >> 8)) & 63) / 31.5f - 1.0f + s3 * 0.35f);
        case 1: return sat((((tt * ((tt >> 9) | (tt >> 13))) & 255) / 127.5f - 1.0f) * 0.62f + s1 * 0.52f);
        case 2: return sat(((((tt >> 4) ^ (tt >> 7) ^ (tt >> 10)) & 255) / 127.5f - 1.0f) * 0.58f + s2 * 0.46f);
        case 3: return sat((sinf(t * 0.00012f * (1.0f + c * 1.7f)) + sinf(t * 0.00031f) * 0.55f + s3 * 1.0f) * 1.2f);
        case 4: return sat((tri * 0.65f + s1 * 0.55f + s2 * 0.45f) * 1.2f);
        case 5: return sat((sinf((t + 4500.0f * s3) * 0.00010f) + sinf(t * 0.000031f) * 1.0f + s2 * 0.55f) * 1.35f);
        case 6: return sat((((((tt >> 6) * (tt >> 3)) & 255) / 127.5f - 1.0f) * 0.28f) + s1 * 0.78f + s3 * 0.62f);
        case 7: return sat((sinf(t * (0.00005f + c * 0.00009f)) + sinf(t * 0.00011f) * sinf(t * 0.000009f) * 1.25f) * 1.18f);
        case 8: return sat((((tt & (tt >> 7) & (tt >> 10)) & 255) / 127.5f - 1.0f) * 0.34f + s2 * 0.74f + s3 * 0.56f);
        case 9: return sat((sinf(t * 0.00008f) + sinf(t * 0.0000813f) + sinf(t * 0.000161f) * 0.78f) * 1.08f);
    }
}

float BytebeatEngine::render(const EngineParams& p) {
    // expansión interna fuerte
    const float morph_wide = 0.20f + p.morph * 12.0f;
    const float color_wide = 0.20f + p.color * 14.0f;
    const float tape_rate = 1.0f - p.tape * 0.97f;

    uint32_t tt = t_;
    if (p.drone) {
        t_ += (uint32_t)(1u + uint32_t(color_wide * 6.0f * tape_rate));
    }

    const float a = render_formula(formula_a_, tt, color_wide);
    const float b = render_formula(formula_b_, tt, color_wide);

    float mix = lerp(a, b, p.morph);

    // morph ahora no solo mezcla: también abre bastante el rango tímbrico
    mix *= (0.65f + morph_wide * 0.12f);
    mix += sinf(float(tt) * 0.00005f * morph_wide) * 0.18f;

    // más low-mid / cuerpo
    float low  = sinf(float(tt) * (0.000020f + color_wide * 0.000010f)) * 0.32f;
    float body = sinf(float(tt) * (0.000055f + color_wide * 0.000020f)) * 0.18f;

    mix = sat(mix * 1.10f + low + body);
    return mix * p.volume;
}

} // namespace synth
