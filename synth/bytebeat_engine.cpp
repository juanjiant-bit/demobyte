
#include "synth/bytebeat_engine.h"
#include <algorithm>
#include <cmath>

namespace synth {
namespace {
static inline float clamp01(float x) {
    return std::clamp(x, 0.0f, 1.0f);
}
static inline float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}
static inline float norm8(uint32_t x) {
    const float v = static_cast<float>(x & 255u) * (1.0f / 127.5f) - 1.0f;
    return std::clamp(v, -1.0f, 1.0f);
}
static inline float tri_from_u32(uint32_t t, uint32_t div) {
    float p = float(t % div) / float(div);
    return 2.0f * fabsf(2.0f * p - 1.0f) - 1.0f;
}
static inline float pulse_from_u32(uint32_t t, uint32_t div, float pw) {
    float p = float(t % div) / float(div);
    return (p < pw) ? 1.0f : -1.0f;
}
}

void BytebeatEngine::init() {
    phase_ = 0.0f;
    sub_phase_ = 0.0f;
    formula_a_ = 0;
    formula_b_ = 7;
    morph_ = 0.0f;
    color_ = 0.0f;
    mod_ = 0.0f;
    pressure_ = 0.0f;
    drone_on_ = true;
    for (int i = 0; i < 6; ++i) history_[i] = 255;
    hist_pos_ = 0;
}

void BytebeatEngine::set_morph(float x) { morph_ = clamp01(x); }
void BytebeatEngine::set_color(float x) { color_ = clamp01(x); }
void BytebeatEngine::set_mod(float x) { mod_ = clamp01(x); }
void BytebeatEngine::set_drone(bool on) { drone_on_ = on; }
void BytebeatEngine::set_pressure(float x) { pressure_ = clamp01(x); }

float BytebeatEngine::rand01() {
    lfsr_ ^= lfsr_ << 13;
    lfsr_ ^= lfsr_ >> 17;
    lfsr_ ^= lfsr_ << 5;
    return (lfsr_ & 0xFFFFu) * (1.0f / 65535.0f);
}

uint8_t BytebeatEngine::pick_formula() {
    // Banco "seguro": evitamos fórmulas demasiado tonales o con nota fija molesta.
    // Usamos 20 fórmulas de 0..21, excluyendo 7 y 9 y 11 y 22/23.
    static const uint8_t safe_bank[20] = {
        0,1,2,3,4,5,
        6,8,10,
        12,13,14,15,16,17,
        18,19,20,21,
        6 // duplicamos una suave en vez de meter una agresiva
    };

    for (int tries = 0; tries < 24; ++tries) {
        uint8_t cand = safe_bank[static_cast<int>(rand01() * 20.0f) % 20];

        bool repeated = false;
        for (int i = 0; i < 6; ++i) {
            if (history_[i] == cand) {
                repeated = true;
                break;
            }
        }
        if (repeated) continue;

        // Con mod bajo, sesgo fuerte a graves/patrones.
        if (mod_ < 0.25f && cand >= 18u && rand01() < 0.85f) continue;
        if (mod_ < 0.45f && cand >= 16u && rand01() < 0.65f) continue;

        history_[hist_pos_] = cand;
        hist_pos_ = (hist_pos_ + 1u) % 6u;
        return cand;
    }

    uint8_t fallback = safe_bank[static_cast<int>(rand01() * 20.0f) % 20];
    history_[hist_pos_] = fallback;
    hist_pos_ = (hist_pos_ + 1u) % 6u;
    return fallback;
}

void BytebeatEngine::randomize_on_boot() {
    next_formula_pair();
    next_formula_pair();
}

void BytebeatEngine::next_formula_pair() {
    formula_a_ = pick_formula();
    formula_b_ = pick_formula();
    if (formula_b_ == formula_a_) {
        formula_b_ = pick_formula();
    }
}

float BytebeatEngine::eval_formula(uint8_t id, uint32_t t) const {
    switch (id % 24u) {
        default:
        case 0:  return norm8(((t >> 6) | (t >> 8) | (t * 3u)));
        case 1:  return 0.75f * tri_from_u32(t, 1536u) + 0.18f * norm8(t >> 5);
        case 2:  return 0.72f * pulse_from_u32(t, 2048u, 0.24f) + 0.20f * tri_from_u32(t, 896u);
        case 3:  return norm8(((t >> 5) * (t >> 8)));
        case 4:  return 0.68f * tri_from_u32(t, 1200u) + 0.20f * pulse_from_u32(t, 700u, 0.12f);
        case 5:  return norm8(((t * 3u & (t >> 10)) | (t * 5u & (t >> 12))));
        case 6: {
            float a = sinf(float(t) * 0.0032f);
            float b = sinf(float(t) * 0.0017f);
            float c = sinf(float(t) * 0.0009f);
            return a * 0.50f + b * 0.28f + c * 0.12f;
        }
        case 7: {
            float a = sinf(float(t) * 0.0035f);
            float b = sinf(float(t) * 0.00525f);
            return (a > b ? 1.0f : -1.0f) * 0.50f + a * 0.16f;
        }
        case 8: {
            float a = tri_from_u32(t, 960u);
            float b = sinf(float(t) * 0.0032f);
            return a * 0.50f + b * 0.28f;
        }
        case 9: {
            float a = sinf(float(t) * 0.0023f);
            float b = sinf(float(t) * 0.00071f);
            return sinf((a + b) * 2.2f) * 0.68f;
        }
        case 10: {
            float a = sinf(float(t) * 0.0014f);
            float b = tri_from_u32(t, 900u);
            return a * 0.34f + b * 0.40f;
        }
        case 11: {
            float a = sinf(float(t) * 0.0048f);
            float b = sinf(float(t) * 0.0032f);
            float c = pulse_from_u32(t, 1500u, 0.22f);
            return a * 0.30f + b * 0.22f + c * 0.24f;
        }
        case 12: return norm8(((t >> 4) | (t >> 7) | (t * 5u))) * 0.46f + sinf(float(t) * 0.0034f) * 0.24f;
        case 13: return norm8(((t * 9u & (t >> 5)) | (t * 3u & (t >> 9)))) * 0.50f + tri_from_u32(t, 640u) * 0.20f;
        case 14: return pulse_from_u32(t, 800u, 0.12f) * 0.36f + sinf(float(t) * 0.007f) * 0.12f + norm8(t >> 5) * 0.16f;
        case 15: return 0.48f * tri_from_u32(t, 520u) + 0.22f * pulse_from_u32(t, 310u, 0.08f);
        case 16: return norm8(((t >> 3) * (t >> 6)) | (t >> 9)) * 0.55f + sinf(float(t) * 0.0018f) * 0.14f;
        case 17: return pulse_from_u32(t, 512u, 0.33f) * 0.24f + tri_from_u32(t, 370u) * 0.30f + sinf(float(t) * 0.008f) * 0.08f;
        case 18: return norm8(((t >> 3) ^ (t >> 6) ^ (t * 7u))) * 0.72f;
        case 19: return norm8(((t * 11u & (t >> 4)) | (t * 7u & (t >> 7)) | (t * 5u & (t >> 10)))) * 0.72f;
        case 20: return norm8(((t * ((t >> 9) | (t >> 13))) & 255u)) * 0.72f;
        case 21: return 0.48f * pulse_from_u32(t, 170u, 0.23f) + 0.30f * tri_from_u32(t, 123u);
        case 22: return sinf(float(t) * 0.016f + sinf(float(t) * 0.0014f) * 1.1f) * 0.64f;
        case 23: return norm8(((t >> 2) ^ (t >> 5) ^ (t >> 8) ^ (t * 9u))) * 0.76f;
    }
}

float BytebeatEngine::softclip(float x) const {
    return x / (1.0f + 0.85f * fabsf(x));
}

float BytebeatEngine::render() {
    const float mod_curve = powf(clamp01(mod_), 0.32f);
    const float live = clamp01(color_);
    const float press = clamp01(pressure_);

    // Pitch base más grave todavía
    const float step =
        0.06f +
        mod_curve * 4.20f +
        live * 0.45f;

    const float tape = 1.0f - 0.78f * press;
    const float adv = step * tape * (drone_on_ ? 1.0f : 1.30f);

    phase_ += adv;
    sub_phase_ += adv * 0.18f;

    const uint32_t t  = static_cast<uint32_t>(phase_);
    const uint32_t ts = static_cast<uint32_t>(sub_phase_);

    const float a = eval_formula(formula_a_, t);
    // Sin offset loco estable: mucho menos sensación de nota pedal/ringmod.
    const float b = eval_formula(formula_b_, t + ts);

    float x = lerp(a, b, morph_);

    // Nada de HP diferencial fuerte. Solo un tilt muy suave.
    const float prev = eval_formula(formula_a_, (t > 0u) ? t - 1u : 0u);
    const float hp = x - 0.12f * prev;
    x = x * (1.0f - 0.22f * live) + hp * (0.22f * live);

    // Sin seno fija pedal. Sumamos cuerpo NO tonal con ramp/tri lenta.
    const float body = tri_from_u32(t, 2400u) * (0.10f + 0.08f * (1.0f - mod_curve));
    x += body;

    const float complexity = 0.94f + 0.52f * mod_curve;
    const float drive = 0.84f + 0.14f * live + 0.16f * press + 0.12f * mod_curve;
    x *= complexity * drive;

    x *= 0.74f;
    return softclip(x);
}

}  // namespace synth
