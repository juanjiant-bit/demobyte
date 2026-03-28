
#include "synth/bytebeat_engine.h"
#include "pico/stdlib.h"
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
static inline float smoothstep(float x) {
    x = clamp01(x);
    return x * x * (3.0f - 2.0f * x);
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
    for (int tries = 0; tries < 20; ++tries) {
        uint8_t cand = static_cast<uint8_t>(rand01() * 24.0f) % 24u;

        bool repeated = false;
        for (int i = 0; i < 6; ++i) {
            if (history_[i] == cand) {
                repeated = true;
                break;
            }
        }
        if (repeated) continue;

        // Low mod: bias strongly toward low, patterned, percussive formulas.
        if (mod_ < 0.25f && cand >= 16u && rand01() < 0.92f) continue;
        if (mod_ < 0.45f && cand >= 20u && rand01() < 0.85f) continue;
        if (mod_ < 0.65f && cand >= 22u && rand01() < 0.70f) continue;

        history_[hist_pos_] = cand;
        hist_pos_ = (hist_pos_ + 1u) % 6u;
        return cand;
    }

    uint8_t fallback = static_cast<uint8_t>(rand01() * 20.0f) % 20u;
    history_[hist_pos_] = fallback;
    hist_pos_ = (hist_pos_ + 1u) % 6u;
    return fallback;
}

void BytebeatEngine::randomize_on_boot() {
    uint32_t s = time_us_32();
    s ^= (s << 7);
    s ^= (s >> 9);
    s ^= 0xA53C9E17u;
    lfsr_ ^= s;
    next_formula_pair();
    next_formula_pair();
}

void BytebeatEngine::next_formula_pair() {
    formula_a_ = pick_formula();
    formula_b_ = pick_formula();
    if (formula_b_ == formula_a_) {
        formula_b_ = (formula_b_ + 5u) % 24u;
    }
}

float BytebeatEngine::eval_formula(uint8_t id, uint32_t t) const {
    switch (id % 24u) {
        // 0..5 = low / chunky / ramp / pulse
        default:
        case 0:  return norm8(((t >> 6) | (t >> 8) | (t * 3u)));
        case 1:  return 0.75f * tri_from_u32(t, 1536u) + 0.18f * norm8(t >> 5);
        case 2:  return 0.72f * pulse_from_u32(t, 2048u, 0.24f) + 0.20f * tri_from_u32(t, 896u);
        case 3:  return norm8(((t >> 5) * (t >> 8)));
        case 4:  return 0.68f * tri_from_u32(t, 1200u) + 0.20f * pulse_from_u32(t, 700u, 0.12f);
        case 5:  return norm8(((t * 3u & (t >> 10)) | (t * 5u & (t >> 12))));

        // 6..11 = float/pattern/melodic
        case 6: {
            float a = sinf(float(t) * 0.0042f);
            float b = sinf(float(t) * 0.0021f);
            float c = sinf(float(t) * 0.0011f);
            return a * 0.55f + b * 0.30f + c * 0.15f;
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
            float a = sinf(float(t) * 0.0018f);
            float b = tri_from_u32(t, 700u);
            return a * 0.38f + b * 0.40f;
        }
        case 11: {
            float a = sinf(float(t) * 0.0048f);
            float b = sinf(float(t) * 0.0032f);
            float c = pulse_from_u32(t, 1500u, 0.22f);
            return a * 0.30f + b * 0.22f + c * 0.24f;
        }

        // 12..17 = hybrid / percussive / ramps
        case 12: return norm8(((t >> 4) | (t >> 7) | (t * 5u))) * 0.46f + sinf(float(t) * 0.0034f) * 0.24f;
        case 13: return norm8(((t * 9u & (t >> 5)) | (t * 3u & (t >> 9)))) * 0.50f + tri_from_u32(t, 640u) * 0.20f;
        case 14: return pulse_from_u32(t, 800u, 0.12f) * 0.36f + sinf(float(t) * 0.007f) * 0.12f + norm8(t >> 5) * 0.16f;
        case 15: return 0.48f * tri_from_u32(t, 520u) + 0.22f * pulse_from_u32(t, 310u, 0.08f);
        case 16: return norm8(((t >> 3) * (t >> 6)) | (t >> 9)) * 0.55f + sinf(float(t) * 0.0018f) * 0.14f;
        case 17: return pulse_from_u32(t, 512u, 0.33f) * 0.24f + tri_from_u32(t, 370u) * 0.30f + sinf(float(t) * 0.008f) * 0.08f;

        // 18..21 = more animated but still controlled
        case 18: return norm8(((t >> 3) ^ (t >> 6) ^ (t * 7u))) * 0.72f;
        case 19: return norm8(((t * 11u & (t >> 4)) | (t * 7u & (t >> 7)) | (t * 5u & (t >> 10)))) * 0.72f;
        case 20: return norm8(((t * ((t >> 9) | (t >> 13))) & 255u)) * 0.72f;
        case 21: return 0.48f * pulse_from_u32(t, 170u, 0.23f) + 0.30f * tri_from_u32(t, 123u);

        // 22..23 = rare / noisy / only at higher mod
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

    // Lower by default. Mod opens up, but floor stays grave.
    const float step =
        0.12f +
        mod_curve * 4.80f +
        live * 0.90f;

    const float tape = 1.0f - 0.78f * press;
    const float adv = step * tape * (drone_on_ ? 1.0f : 1.35f);

    phase_ += adv;
    sub_phase_ += adv * 0.23f;

    const uint32_t t  = static_cast<uint32_t>(phase_);
    const uint32_t ts = static_cast<uint32_t>(sub_phase_);

    const float a = eval_formula(formula_a_, t);
    const float b = eval_formula(formula_b_, t + (ts << 2));
    float x = lerp(a, b, morph_);

    const float prev = eval_formula(formula_a_, (t > 0u) ? t - 1u : 0u);
    const float hp = x - 0.26f * prev;

    // Much gentler tilt to avoid ring-mod/FM noise feel.
    x = x * (1.0f - 0.46f * live) + hp * (0.46f * live);

    // More grounded body by default
    const float body = sinf(float(t) * 0.00075f) * (0.24f + 0.16f * (1.0f - mod_curve));
    x += body;

    const float complexity = 0.92f + 0.62f * mod_curve;
    const float drive = 0.82f + 0.18f * live + 0.18f * press + 0.16f * mod_curve;
    x *= complexity * drive;

    x *= 0.72f;
    return softclip(x);
}

}  // namespace synth
