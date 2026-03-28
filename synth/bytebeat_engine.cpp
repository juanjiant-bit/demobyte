
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
}

void BytebeatEngine::init() {
    t_ = 0;
    phase_f_ = 0.0f;
    formula_a_ = 0;
    formula_b_ = 1;
    morph_ = 0.0f;
    color_ = 0.5f;
    pressure_ = 0.0f;
    drone_on_ = false;
    recent_[0] = recent_[1] = recent_[2] = recent_[3] = 255u;
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

void BytebeatEngine::next_formula_pair() {
    constexpr uint8_t kBankSize = 18u;

    auto next_rand = [this]() -> uint32_t {
        lfsr_ ^= lfsr_ << 13;
        lfsr_ ^= lfsr_ >> 17;
        lfsr_ ^= lfsr_ << 5;
        return lfsr_;
    };

    auto recently_used = [this](uint8_t v) -> bool {
        for (uint8_t i = 0; i < 4; ++i) {
            if (recent_[i] == v) return true;
        }
        return false;
    };

    uint8_t a = formula_a_;
    for (int tries = 0; tries < 24; ++tries) {
        uint8_t cand = static_cast<uint8_t>(next_rand() % kBankSize);
        if (cand == formula_a_) continue;
        if (recently_used(cand)) continue;
        a = cand;
        break;
    }

    uint8_t b = formula_b_;
    for (int tries = 0; tries < 24; ++tries) {
        uint8_t cand = static_cast<uint8_t>(next_rand() % kBankSize);
        if (cand == a) continue;
        if (recently_used(cand)) continue;
        b = cand;
        break;
    }

    formula_a_ = a;
    formula_b_ = b;
    recent_[recent_idx_ & 3u] = formula_a_;
    recent_idx_ = (recent_idx_ + 1u) & 3u;
    recent_[recent_idx_ & 3u] = formula_b_;
    recent_idx_ = (recent_idx_ + 1u) & 3u;
}

float BytebeatEngine::eval_formula(uint8_t id, uint32_t t) const {
    switch (id % 18u) {
        // 0..5 = las originales de BUENA
        default:
        case 0: return norm8(((t >> 4) | (t >> 5) | (t * 3u)));
        case 1: return norm8(((t * 5u & (t >> 7)) | (t * 3u & (t >> 10))));
        case 2: return norm8((t * (((t >> 11) & (t >> 8)) & 123u & (t >> 3))));
        case 3: return norm8((((t >> 3) * (t >> 5)) | (t >> 7)));
        case 4: return norm8(((t * 9u & (t >> 4)) | (t * 5u & (t >> 7)) | (t * 3u & (t >> 10))));
        case 5: return norm8(((t >> 4) ^ (t >> 7) ^ (t * 3u)));

        // 6..11 = más percusivas / rampas / pulsos
        case 6: return 0.75f * tri_u32(t, 1536u) + 0.20f * norm8(t >> 5);
        case 7: return 0.72f * pulse_u32(t, 1800u, 0.18f) + 0.18f * tri_u32(t, 760u);
        case 8: return norm8(((t >> 5) * (t >> 9)) ^ (t >> 7));
        case 9: return 0.55f * tri_u32(t, 1100u) + 0.28f * pulse_u32(t, 620u, 0.10f);
        case 10: return norm8(((t * 7u & (t >> 6)) | (t * 3u & (t >> 11))));
        case 11: return 0.48f * tri_u32(t, 420u) + 0.22f * norm8((t >> 3) ^ (t >> 8));

        // 12..17 = más evolutivas / float-ish pero seguras
        case 12: {
            float a = sinf(float(t) * 0.0045f);
            float b = sinf(float(t) * 0.0021f);
            return a * 0.58f + b * 0.24f;
        }
        case 13: {
            float a = sinf(float(t) * 0.0032f);
            float b = tri_u32(t, 980u);
            return a * 0.42f + b * 0.34f;
        }
        case 14: {
            float a = sinf(float(t) * 0.0018f);
            float b = sinf(float(t) * 0.0009f);
            return sinf((a + b) * 2.1f) * 0.68f;
        }
        case 15: return 0.30f * pulse_u32(t, 340u, 0.25f) + 0.42f * tri_u32(t, 900u);
        case 16: return norm8(((t >> 6) | (t * 5u) | (t >> 9)));
        case 17: {
            float a = sinf(float(t) * 0.0058f);
            float b = pulse_u32(t, 1500u, 0.20f);
            return a * 0.34f + b * 0.26f;
        }
    }
}

float BytebeatEngine::softclip(float x) const {
    return x / (1.0f + 0.75f * fabsf(x));
}

float BytebeatEngine::render() {
    // Centro del pote = comportamiento parecido a BUENA.
    // Extremos = ~3x más abajo y ~3x más arriba.
    const float centered = (color_ - 0.5f) * 2.0f;        // -1 .. +1
    const float ratio = powf(3.0f, centered);             // 1/3 .. 3
    const float base_rate = drone_on_ ? 1.5f : 3.0f;      // centro similar al original
    const float pressure_pull = 1.0f - 0.55f * pressure_; // aftertouch frena pero no mata
    const float step = base_rate * ratio * pressure_pull;

    phase_f_ += step;
    if (phase_f_ < 0.0f) phase_f_ = 0.0f;
    t_ = static_cast<uint32_t>(phase_f_);

    const float a = eval_formula(formula_a_, t_);
    const float b = eval_formula(formula_b_, t_);
    float x = a + (b - a) * morph_;

    // Mantiene el carácter original pero más controlado.
    const float prev = eval_formula(formula_a_, (t_ > 0u) ? t_ - 1u : 0u);
    const float hp = x - 0.24f * prev;
    x = x * (1.0f - 0.45f * color_) + hp * (0.45f * color_);

    // Drive un poco más amplio, pero no tan harsh.
    float drive = 0.82f + 0.42f * color_ + 0.22f * pressure_;
    x *= drive;

    x *= 0.78f;
    return softclip(x);
}

}  // namespace synth
