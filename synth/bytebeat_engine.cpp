
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

static inline float tri_u32(uint32_t t, uint32_t period) {
    float p = float(t % period) / float(period);
    return 2.0f * fabsf(2.0f * p - 1.0f) - 1.0f;
}

static inline float pulse_u32(uint32_t t, uint32_t period, float pw) {
    float p = float(t % period) / float(period);
    return (p < pw) ? 1.0f : -1.0f;
}

static inline float fractf(float x) {
    return x - floorf(x);
}

static inline float tri_f(float x) {
    float p = fractf(x);
    return 2.0f * fabsf(2.0f * p - 1.0f) - 1.0f;
}

static inline float sat(float x) {
    return x / (1.0f + 0.8f * fabsf(x));
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
    recent_[0] = recent_[1] = recent_[2] = recent_[3] = recent_[4] = recent_[5] = 255u;
    recent_idx_ = 0;
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

uint32_t BytebeatEngine::next_rand() {
    lfsr_ ^= lfsr_ << 13;
    lfsr_ ^= lfsr_ >> 17;
    lfsr_ ^= lfsr_ << 5;
    return lfsr_;
}

void BytebeatEngine::next_formula_pair() {
    constexpr uint8_t kBankSize = 16u;

    auto recently_used = [this](uint8_t v) -> bool {
        for (uint8_t i = 0; i < 6; ++i) {
            if (recent_[i] == v) return true;
        }
        return false;
    };

    uint8_t a = formula_a_;
    for (int tries = 0; tries < 32; ++tries) {
        uint8_t cand = static_cast<uint8_t>(next_rand() % kBankSize);
        if (cand == formula_a_) continue;
        if (recently_used(cand)) continue;
        a = cand;
        break;
    }

    uint8_t b = formula_b_;
    for (int tries = 0; tries < 32; ++tries) {
        uint8_t cand = static_cast<uint8_t>(next_rand() % kBankSize);
        if (cand == a) continue;
        if (recently_used(cand)) continue;
        b = cand;
        break;
    }

    formula_a_ = a;
    formula_b_ = b;
    recent_[recent_idx_ % 6u] = formula_a_;
    recent_idx_ = (recent_idx_ + 1u) % 6u;
    recent_[recent_idx_ % 6u] = formula_b_;
    recent_idx_ = (recent_idx_ + 1u) % 6u;
}

float BytebeatEngine::eval_formula(uint8_t id, uint32_t t) const {
    const float tf = float(t);

    switch (id % 16u) {
        // 0..5: base BUENA
        default:
        case 0: return norm8(((t >> 4) | (t >> 5) | (t * 3u)));
        case 1: return norm8(((t * 5u & (t >> 7)) | (t * 3u & (t >> 10))));
        case 2: return norm8((t * (((t >> 11) & (t >> 8)) & 123u & (t >> 3))));
        case 3: return norm8((((t >> 3) * (t >> 5)) | (t >> 7)));
        case 4: return norm8(((t * 9u & (t >> 4)) | (t * 5u & (t >> 7)) | (t * 3u & (t >> 10))));
        case 5: return norm8(((t >> 4) ^ (t >> 7) ^ (t * 3u)));

        // 6..9: inspiradas en capturas "ramp/pulse/trippy", más musicales
        case 6: return 0.78f * tri_u32(t, 1536u) + 0.16f * pulse_u32(t, 760u, 0.15f);
        case 7: return sat(0.60f * tri_u32(t, 980u) + 0.26f * sinf(tf * 0.0032f));
        case 8: return sat(0.52f * pulse_u32(t, 1900u, 0.18f) + 0.24f * tri_u32(t, 620u));
        case 9: return sat(norm8(((t >> 5) * (t >> 9)) ^ (t >> 7)) * 0.85f + 0.18f * tri_u32(t, 1300u));

        // 10..12: inspiradas en capturas floatbeat / evolutivas
        case 10: {
            // basada en la idea de la captura "Trippy test"
            float a = sinf(tf * 0.0045f);
            float b = sinf(tf * 0.0021f);
            float c = tri_f(tf * 0.00042f);
            return sat(a * 0.48f + b * 0.24f + c * 0.18f);
        }
        case 11: {
            float a = sinf(tf * 0.0032f);
            float b = sinf((a + 1.2f) * tf * 0.00022f);
            float c = tri_u32(t, 900u);
            return sat(a * 0.34f + b * 0.28f + c * 0.22f);
        }
        case 12: {
            float a = sinf(tf * 0.0018f);
            float b = sinf(tf * 0.0009f);
            return sat(sinf((a + b) * 2.2f) * 0.72f + tri_u32(t, 2100u) * 0.12f);
        }

        // 13: inspirada en la captura con drums internos pero suavizada
        case 13: {
            float kick = sinf(15.0f * sqrtf(fractf(tf / 16384.0f))) * powf(1.0f - fractf(tf / 16384.0f), 2.6f);
            float hat = (float(int((t * 1103515245u + 12345u) >> 24) & 255) / 127.5f - 1.0f) * powf(1.0f - fractf(tf / 8192.0f), 3.0f);
            return sat(kick * 0.58f + hat * 0.16f + tri_u32(t, 700u) * 0.14f);
        }

        // 14..15: nuevas capas para abrir paleta sin harsh constante
        case 14: {
            float a = pulse_u32(t, 340u, 0.25f);
            float b = tri_u32(t, 900u);
            return sat(a * 0.28f + b * 0.42f + sinf(tf * 0.0012f) * 0.16f);
        }
        case 15: {
            float a = sinf(tf * 0.0058f);
            float b = pulse_u32(t, 1500u, 0.20f);
            return sat(a * 0.34f + b * 0.26f + tri_u32(t, 2400u) * 0.12f);
        }
    }
}

float BytebeatEngine::softclip(float x) const {
    return x / (1.0f + 0.75f * fabsf(x));
}

float BytebeatEngine::render() {
    // Centro = comportamiento parecido a BUENA.
    // Extremos = aprox 3x abajo y 3x arriba.
    const float centered = (color_ - 0.5f) * 2.0f; // -1..1
    const float ratio = powf(3.0f, centered);      // 1/3..3
    const float base = drone_on_ ? 1.5f : 3.0f;
    const float slow_from_pressure = 1.0f - 0.55f * pressure_;
    const float step = base * ratio * slow_from_pressure;

    t_ += static_cast<uint32_t>(std::max(1.0f, step));

    const float a = eval_formula(formula_a_, t_);
    const float b = eval_formula(formula_b_, t_ + 17u);
    float x = a + (b - a) * morph_;

    // Tilt más suave para no volverlo harsh / mosquito
    const float prev = eval_formula(formula_a_, (t_ > 0u) ? t_ - 1u : 0u);
    const float hp = x - 0.18f * prev;
    x = x * (1.0f - 0.34f * color_) + hp * (0.34f * color_);

    float drive = 0.82f + 0.34f * color_ + 0.20f * pressure_;
    x *= drive;

    x *= 0.78f;
    return softclip(x);
}

}  // namespace synth
