
#include "synth/bytebeat_engine.h"
#include <cmath>
#include <algorithm>

namespace synth {

static inline float clamp01(float x){
    return std::clamp(x, 0.0f, 1.0f);
}

float BytebeatEngine::render() {

    // 🔥 MÁS GRAVE todavía
    float step = 0.08f + mod_ * 3.5f + color_ * 0.6f;

    phase_ += step;
    uint32_t t = (uint32_t)phase_;

    float a = eval_formula(formula_a_, t);
    float b = eval_formula(formula_b_, t);

    float x = a + (b - a) * morph_;

    // 🔥 sacar ese “ring mod feel”
    // NO usar diferencia fuerte tipo hp
    // suavizar mezcla
    x *= 0.7f;

    // 🔥 cuerpo grave extra (mata la nota mosquito)
    float body = sinf((float)t * 0.0006f) * 0.35f;
    x += body;

    // 🔥 saturación suave
    x = x * 1.4f;
    x = x / (1.0f + fabsf(x));

    return x;
}

}  // namespace synth
