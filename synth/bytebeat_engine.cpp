
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

static inline float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

static inline float smoothstep(float x) {
    x = clamp01(x);
    return x * x * (3.0f - 2.0f * x);
}
}

void BytebeatEngine::init() {
    phase_ = 0.0f;
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

float BytebeatEngine::zone_shape(float x, int zone, float zf) const {
    switch (zone) {
        default:
        case 0: {
            // ultra slow / rounded / drifting
            const float drift = sinf(phase_ * 0.000021f) * (0.08f + 0.12f * zf);
            return x * (0.62f + 0.18f * zf) + drift;
        }
        case 1: {
            // pulsing lfo-ish with xor-ish tremor
            const float trem = sinf(phase_ * 0.00017f) * (0.10f + 0.22f * zf);
            return x * (0.78f + 0.18f * zf) + trem;
        }
        case 2: {
            // musically animated mid-rate
            const float ring = sinf(phase_ * (0.0008f + 0.0025f * zf));
            return x * (0.88f + 0.28f * zf) + ring * (0.12f + 0.18f * zf);
        }
        case 3: {
            // fast stepping / crunchy logic
            return x * (1.00f + 0.45f * zf);
        }
        case 4: {
            // audio-rate harsh / FM-ish
            const float fm = sinf(phase_ * (0.01f + 0.08f * zf));
            return x * (1.10f + 0.70f * zf) + fm * (0.14f + 0.26f * zf);
        }
    }
}

float BytebeatEngine::render() {
    // Pot 3 (mod_) is now a huge rate sweep with zones.
    // zone 0 = ultra slow LFO-long movements
    // zone 4 = audio-rate / aggressive.
    const float m = clamp01(mod_);
    const float live = clamp01(color_);      // aftertouch live color / rate feel
    const float press = clamp01(pressure_);

    int zone = 0;
    float zf = 0.0f;
    float step = 1.0f;

    if (m < 0.20f) {
        zone = 0;
        zf = smoothstep(m / 0.20f);
        step = lerp(0.003f, 0.08f, zf);
    } else if (m < 0.40f) {
        zone = 1;
        zf = smoothstep((m - 0.20f) / 0.20f);
        step = lerp(0.08f, 0.60f, zf);
    } else if (m < 0.60f) {
        zone = 2;
        zf = smoothstep((m - 0.40f) / 0.20f);
        step = lerp(0.60f, 3.0f, zf);
    } else if (m < 0.80f) {
        zone = 3;
        zf = smoothstep((m - 0.60f) / 0.20f);
        step = lerp(3.0f, 20.0f, zf);
    } else {
        zone = 4;
        zf = smoothstep((m - 0.80f) / 0.20f);
        step = lerp(20.0f, 120.0f, zf);
    }

    // aftertouch still does the live tape/color behavior on top
    step *= (0.18f + 1.10f * live);
    step *= (1.0f - 0.90f * press);

    if (!drone_on_) {
        step *= 1.6f;
    }

    phase_ += step;
    if (phase_ < 0.0f) phase_ = 0.0f;

    // time warps by zone to widen the fan of behaviors
    uint32_t t0 = static_cast<uint32_t>(phase_);
    uint32_t ta = t0;
    uint32_t tb = t0;

    switch (zone) {
        case 0:
            ta = static_cast<uint32_t>(phase_ * (1.0f + 0.03f * zf));
            tb = static_cast<uint32_t>(phase_ * (1.0f + 0.09f * zf));
            break;
        case 1:
            ta = t0 ^ (t0 >> (2 + int(2 * zf)));
            tb = t0 + static_cast<uint32_t>(phase_ * (0.5f + 0.5f * zf));
            break;
        case 2:
            ta = t0 + static_cast<uint32_t>(phase_ * (1.0f + 0.5f * zf));
            tb = (t0 ^ (t0 >> 3)) + static_cast<uint32_t>(phase_ * (0.2f + 0.8f * zf));
            break;
        case 3:
            ta = (t0 * (2u + static_cast<uint32_t>(4.0f * zf))) ^ (t0 >> 2);
            tb = (t0 + (t0 >> 1)) ^ (t0 >> (2 + int(2 * zf)));
            break;
        case 4:
            ta = (t0 * (6u + static_cast<uint32_t>(14.0f * zf))) ^ (t0 >> 1);
            tb = (t0 * (3u + static_cast<uint32_t>(9.0f * zf))) ^ (t0 >> (1 + int(2 * zf)));
            break;
    }

    const float a = eval_formula(formula_a_, ta);
    const float b = eval_formula(formula_b_, tb);
    float x = a + (b - a) * morph_;

    // live color still does the immediate bright/dark tilt
    const float prev = eval_formula(formula_a_, ta > 0 ? ta - 1u : ta);
    const float hp = x - 0.35f * prev;
    x = x * (1.0f - 0.72f * live) + hp * (0.72f * live);

    // zone-dependent shaping over the whole mod sweep
    x = zone_shape(x, zone, zf);

    // body grows in lower/mid zones, aggression grows in upper zones
    const float body = sinf(0.00011f * phase_) * (0.08f + 0.22f * (1.0f - clamp01(m * 1.2f)));
    const float air  = sinf(0.0017f * phase_) * (0.02f + 0.16f * clamp01((m - 0.55f) * 2.2f));
    x += body + air;

    const float drive = 0.78f + 0.42f * live + 0.24f * press + 0.44f * m;
    x *= drive;

    x *= 0.70f;
    return softclip(x);
}

}  // namespace synth
