#include "bytebeat_engine.h"
#include <cmath>

namespace synth {
namespace {
static inline float sat(float x) { return x / (1.0f + fabsf(x)); }
static inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
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
    const float s1 = sinf(t * (0.00035f + c * 0.00012f));
    const float s2 = sinf(t * (0.00019f + c * 0.00008f));
    const float s3 = sinf(t * (0.00007f + c * 0.00003f));
    const float tri = 2.0f * fabsf(fmodf(t * (0.00011f + c * 0.00006f), 1.0f) - 0.5f) - 1.0f;

    switch (id) {
        default:
        case 0: return sat((((tt >> 5) | (tt >> 8)) & 63) / 31.5f - 1.0f + s3 * 0.30f);
        case 1: return sat((((tt * ((tt >> 9) | (tt >> 13))) & 255) / 127.5f - 1.0f) * 0.7f + s1 * 0.45f);
        case 2: return sat(((((tt >> 4) ^ (tt >> 7) ^ (tt >> 10)) & 255) / 127.5f - 1.0f) * 0.65f + s2 * 0.35f);
        case 3: return sat((sinf(t * 0.00022f * (1.0f + c * 1.8f)) + sinf(t * 0.00037f) * 0.55f + s3 * 0.9f) * 1.2f);
        case 4: return sat((tri * 0.7f + s1 * 0.6f + s2 * 0.35f) * 1.25f);
        case 5: return sat((sinf((t + 4000.0f * s3) * 0.00016f) + sinf(t * 0.000043f) * 0.9f + s2 * 0.45f) * 1.35f);
        case 6: return sat((((((tt >> 6) * (tt >> 3)) & 255) / 127.5f - 1.0f) * 0.35f) + s1 * 0.7f + s3 * 0.5f);
        case 7: return sat((sinf(t * (0.00009f + c * 0.00006f)) + sinf(t * 0.00017f) * sinf(t * 0.000013f) * 1.2f) * 1.25f);
        case 8: return sat((((tt & (tt >> 7) & (tt >> 10)) & 255) / 127.5f - 1.0f) * 0.45f + s2 * 0.65f + s3 * 0.45f);
        case 9: return sat((sinf(t * 0.00012f) + sinf(t * 0.0001213f) + sinf(t * 0.000241f) * 0.65f) * 1.05f);
    }
}

float BytebeatEngine::render(const EngineParams& p) {
    const float tape_rate = 1.0f - p.tape * 0.97f;
    float c = p.color;
    if (c < 0.0f) c = 0.0f;
    if (c > 1.0f) c = 1.0f;

    uint32_t tt = t_;
    if (p.drone) t_ += (uint32_t)(1u + uint32_t(c * 5.0f * tape_rate));

    const float a = render_formula(formula_a_, tt, c);
    const float b = render_formula(formula_b_, tt, c);
    float mix = lerp(a, b, p.morph);
    float low = sinf(float(tt) * (0.000035f + c * 0.00001f)) * 0.25f;
    float body = sinf(float(tt) * (0.00009f + c * 0.00002f)) * 0.12f;
    mix = sat(mix * 1.15f + low + body);
    return mix * p.volume;
}

} // namespace synth
