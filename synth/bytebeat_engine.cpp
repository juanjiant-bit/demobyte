
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
static inline float smoothstep(float x) {
    x = clamp01(x);
    return x * x * (3.0f - 2.0f * x);
}
static inline float norm8(uint32_t x) {
    const float v = static_cast<float>(x & 255u) * (1.0f / 127.5f) - 1.0f;
    return std::clamp(v, -1.0f, 1.0f);
}
static inline float tri01(float p) {
    p -= floorf(p);
    return 2.0f * fabsf(2.0f * p - 1.0f) - 1.0f;
}
static inline float pulse01(float p, float pw) {
    p -= floorf(p);
    return (p < pw) ? 1.0f : -1.0f;
}
}

void BytebeatEngine::init() {
    phase_ = 0.0f;
    aux_phase_ = 0.0f;
    aux2_phase_ = 0.0f;
    perc_env_a_ = 0.0f;
    perc_env_b_ = 0.0f;
    slot_a_ = {BYTE_LOGIC, 0};
    slot_b_ = {FLOAT_PATTERN, 0};
    morph_ = 0.0f;
    color_ = 0.0f;
    mod_ = 0.0f;
    pressure_ = 0.0f;
    drone_on_ = false;
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
    auto pick_slot = [this]() -> AlgoSlot {
        AlgoSlot slot;
        slot.kind = static_cast<AlgoKind>(static_cast<uint8_t>(rand01() * 4.0f) % 4u);
        switch (slot.kind) {
            case BYTE_LOGIC:    slot.variant = static_cast<uint8_t>(rand01() * 6.0f) % 6u; break;
            case FLOAT_PATTERN: slot.variant = static_cast<uint8_t>(rand01() * 6.0f) % 6u; break;
            case FLOAT_PERC:    slot.variant = static_cast<uint8_t>(rand01() * 4.0f) % 4u; break;
            case HYBRID:        slot.variant = static_cast<uint8_t>(rand01() * 5.0f) % 5u; break;
            default:            slot.variant = 0; break;
        }
        return slot;
    };

    AlgoSlot a = pick_slot();
    AlgoSlot b = pick_slot();

    // evitar mismo slot exacto
    if (a.kind == b.kind && a.variant == b.variant) {
        b.variant = static_cast<uint8_t>(b.variant + 1u);
    }

    slot_a_ = a;
    slot_b_ = b;
}

float BytebeatEngine::eval_byte_logic(uint8_t variant, uint32_t t) const {
    switch (variant % 6u) {
        default:
        case 0: return norm8(((t >> 4) | (t >> 5) | (t * 3u)));
        case 1: return norm8(((t * 5u & (t >> 7)) | (t * 3u & (t >> 10))));
        case 2: return norm8((t * (((t >> 11) & (t >> 8)) & 123u & (t >> 3))));
        case 3: return norm8((((t >> 3) * (t >> 5)) | (t >> 7)));
        case 4: return norm8(((t * 9u & (t >> 4)) | (t * 5u & (t >> 7)) | (t * 3u & (t >> 10))));
        case 5: return norm8(((t >> 4) ^ (t >> 7) ^ (t * 3u)));
    }
}

float BytebeatEngine::eval_float_pattern(uint8_t variant, float ph, float rate_hz, float live, float zone) const {
    const float p1 = ph;
    const float p2 = ph * (0.5f + 0.25f * variant);
    const float p3 = ph * (1.0f + 0.333f * (variant + 1));
    const float s1 = sinf(6.2831853f * p1);
    const float s2 = sinf(6.2831853f * p2);
    const float s3 = sinf(6.2831853f * p3);

    switch (variant % 6u) {
        default:
        case 0:
            return (s1 * 0.55f + s2 * 0.35f + s3 * 0.18f);
        case 1:
            return ((s1 > s2 ? 1.0f : -1.0f) * 0.55f + s3 * 0.28f);
        case 2:
            return (tri01(p1) * 0.52f + sinf(6.2831853f * p2) * 0.33f + pulse01(p3, 0.34f) * 0.12f);
        case 3:
            return (sinf(6.2831853f * (p1 + 0.15f * s2)) * 0.62f + s3 * 0.24f);
        case 4:
            return ((s1 + s2) * (0.35f + 0.10f * zone) + tri01(p3) * 0.26f);
        case 5:
            return ((s1 > 0.15f ? 1.0f : -1.0f) * 0.42f + sinf(6.2831853f * (p2 * 0.75f)) * 0.36f + s3 * 0.12f);
    }
}

float BytebeatEngine::eval_float_perc(uint8_t variant, float ph, float rate_hz, float live, float zone) const {
    float x = 0.0f;
    switch (variant % 4u) {
        default:
        case 0: {
            const float g = pulse01(ph * 0.5f, 0.08f);
            x = g * 0.55f + sinf(6.2831853f * ph * 2.0f) * 0.25f;
            break;
        }
        case 1: {
            const float g = (sinf(6.2831853f * ph) > 0.86f) ? 1.0f : -1.0f;
            x = g * 0.48f + tri01(ph * 3.0f) * 0.22f;
            break;
        }
        case 2: {
            const float p = pulse01(ph * 1.25f, 0.18f);
            x = p * 0.40f + sinf(6.2831853f * ph * 5.0f) * 0.18f;
            break;
        }
        case 3: {
            const float inter = sinf(6.2831853f * ph) > sinf(6.2831853f * ph * 1.5f) ? 1.0f : -1.0f;
            x = inter * 0.42f + tri01(ph * 2.5f) * 0.20f;
            break;
        }
    }
    return x;
}

float BytebeatEngine::eval_hybrid(uint8_t variant, uint32_t t, float ph, float rate_hz, float live, float zone, float morph) const {
    const float bb = eval_byte_logic(variant % 6u, t);
    const float fp = eval_float_pattern(variant % 6u, ph, rate_hz, live, zone);
    return lerp(bb, fp, 0.45f + 0.25f * morph);
}

float BytebeatEngine::eval_slot(const AlgoSlot& slot, float ph, float rate_hz, float live, float zone, float morph) const {
    const uint32_t t = static_cast<uint32_t>(ph * 65536.0f);
    switch (slot.kind) {
        case BYTE_LOGIC:    return eval_byte_logic(slot.variant, t);
        case FLOAT_PATTERN: return eval_float_pattern(slot.variant, ph, rate_hz, live, zone);
        case FLOAT_PERC:    return eval_float_perc(slot.variant, ph, rate_hz, live, zone);
        case HYBRID:        return eval_hybrid(slot.variant, t, ph, rate_hz, live, zone, morph);
        default:            return 0.0f;
    }
}

float BytebeatEngine::softclip(float x) const {
    return x / (1.0f + 0.75f * fabsf(x));
}

float BytebeatEngine::render() {
    const float m = clamp01(mod_);
    const float live = clamp01(color_);
    const float press = clamp01(pressure_);

    int zone = 0;
    float zf = 0.0f;
    float rate_hz = 0.01f;

    if (m < 0.20f) {
        zone = 0;
        zf = smoothstep(m / 0.20f);
        rate_hz = lerp(0.01f, 0.15f, zf);      // ultra lento / LFO largo
    } else if (m < 0.40f) {
        zone = 1;
        zf = smoothstep((m - 0.20f) / 0.20f);
        rate_hz = lerp(0.15f, 1.5f, zf);
    } else if (m < 0.60f) {
        zone = 2;
        zf = smoothstep((m - 0.40f) / 0.20f);
        rate_hz = lerp(1.5f, 8.0f, zf);
    } else if (m < 0.80f) {
        zone = 3;
        zf = smoothstep((m - 0.60f) / 0.20f);
        rate_hz = lerp(8.0f, 45.0f, zf);
    } else {
        zone = 4;
        zf = smoothstep((m - 0.80f) / 0.20f);
        rate_hz = lerp(45.0f, 4200.0f, zf);    // audio rate alto
    }

    // aftertouch = tape/rate-down + color vivo
    rate_hz *= (0.12f + 1.05f * live);
    rate_hz *= (1.0f - 0.92f * press);
    if (!drone_on_) rate_hz *= 1.6f;
    if (rate_hz < 0.001f) rate_hz = 0.001f;

    phase_ += rate_hz / 44100.0f;
    aux_phase_ += (rate_hz * (0.5f + 0.9f * zf)) / 44100.0f;
    aux2_phase_ += (rate_hz * (1.5f + 1.2f * morph_)) / 44100.0f;

    // reinyectar envs percusivas a baja tasa
    const float edge_a = sinf(6.2831853f * phase_) > 0.92f ? 1.0f : 0.0f;
    const float edge_b = sinf(6.2831853f * aux_phase_) > 0.94f ? 1.0f : 0.0f;
    if (edge_a > 0.5f) perc_env_a_ = 1.0f;
    if (edge_b > 0.5f) perc_env_b_ = 1.0f;
    perc_env_a_ *= 0.9975f;
    perc_env_b_ *= 0.9965f;

    const float a = eval_slot(slot_a_, phase_, rate_hz, live, zf, morph_);
    const float b = eval_slot(slot_b_, aux_phase_ + 0.21f * sinf(6.2831853f * aux2_phase_), rate_hz, live, zf, morph_);
    float x = lerp(a, b, morph_);

    // tilt vivo desde aftertouch
    const float hp_ref = eval_slot(slot_a_, phase_ - (rate_hz / 44100.0f), rate_hz, live, zf, morph_);
    const float hp = x - 0.35f * hp_ref;
    x = x * (1.0f - 0.72f * live) + hp * (0.72f * live);

    // comportamiento por zonas
    if (zone <= 1) {
        const float body = sinf(6.2831853f * (phase_ * 0.12f)) * (0.18f + 0.20f * (1.0f - m));
        x += body;
    } else if (zone == 2) {
        x += pulse01(aux_phase_ * 0.5f, 0.32f) * 0.10f;
    } else if (zone == 3) {
        x += sinf(6.2831853f * aux2_phase_) * 0.12f;
    } else {
        x += sinf(6.2831853f * aux2_phase_) * (0.10f + 0.18f * zf);
    }

    // usar env percusiva para que floatbeats no sean solo "ruido fm"
    x += (perc_env_a_ * 0.22f + perc_env_b_ * 0.16f) * (0.3f + 0.4f * (1.0f - m));

    const float drive = 0.76f + 0.40f * live + 0.24f * press + 0.34f * m;
    x *= drive;

    x *= 0.72f;
    return softclip(x);
}

}  // namespace synth
