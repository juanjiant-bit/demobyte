
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
static inline float tri(float p) {
    p -= floorf(p);
    return 2.0f * fabsf(2.0f * p - 1.0f) - 1.0f;
}
static inline float pulse(float p, float pw) {
    p -= floorf(p);
    return (p < pw) ? 1.0f : -1.0f;
}
}

void BytebeatEngine::init() {
    motion_phase_ = 0.0f;
    carrier_phase_ = 0.0f;
    carrier2_phase_ = 0.0f;
    env_a_ = 0.0f;
    env_b_ = 0.0f;
    slot_a_ = {BYTE_LOGIC, 0};
    slot_b_ = {FLOAT_PATTERN, 0};
    morph_ = 0.0f;
    color_ = 1.0f;
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
    auto pick = [this]() -> AlgoSlot {
        AlgoSlot slot;
        slot.kind = static_cast<AlgoKind>(static_cast<uint8_t>(rand01() * 4.0f) % 4u);
        switch (slot.kind) {
            case BYTE_LOGIC:    slot.variant = static_cast<uint8_t>(rand01() * 6.0f) % 6u; break;
            case FLOAT_PATTERN: slot.variant = static_cast<uint8_t>(rand01() * 8.0f) % 8u; break;
            case FLOAT_PERC:    slot.variant = static_cast<uint8_t>(rand01() * 5.0f) % 5u; break;
            case HYBRID:        slot.variant = static_cast<uint8_t>(rand01() * 6.0f) % 6u; break;
            default:            slot.variant = 0; break;
        }
        return slot;
    };

    slot_a_ = pick();
    slot_b_ = pick();
    if (slot_a_.kind == slot_b_.kind && slot_a_.variant == slot_b_.variant) {
        slot_b_.variant = static_cast<uint8_t>(slot_b_.variant + 1u);
    }
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

float BytebeatEngine::eval_float_pattern(uint8_t variant, float ph, float carrier_hz) const {
    const float p = ph - floorf(ph);
    const float a = sinf(6.2831853f * p);
    const float b = sinf(6.2831853f * p * (1.5f + 0.25f * variant));
    const float c = sinf(6.2831853f * p * (0.5f + 0.12f * variant));

    switch (variant % 8u) {
        default:
        case 0: return a * 0.55f + b * 0.28f + c * 0.18f;
        case 1: return (a > b ? 1.0f : -1.0f) * 0.52f + c * 0.24f;
        case 2: return tri(p * (1.0f + 0.25f * variant)) * 0.48f + b * 0.24f + pulse(p * 2.0f, 0.33f) * 0.12f;
        case 3: return sinf(6.2831853f * (p + 0.15f * b)) * 0.60f + c * 0.20f;
        case 4: return (a + b) * 0.30f + tri(p * 0.75f) * 0.30f + c * 0.16f;
        case 5: return pulse(p * (1.0f + 0.3f * variant), 0.22f) * 0.40f + sinf(6.2831853f * p * 0.5f) * 0.25f;
        case 6: return (sinf(6.2831853f * p * 2.0f) > sinf(6.2831853f * p * 3.0f) ? 1.0f : -1.0f) * 0.42f + b * 0.18f;
        case 7: return sinf(6.2831853f * p * 0.25f) * 0.35f + sinf(6.2831853f * p * 1.25f) * 0.32f + tri(p * 2.0f) * 0.12f;
    }
}

float BytebeatEngine::eval_float_perc(uint8_t variant, float ph, float carrier_hz) const {
    const float p = ph - floorf(ph);
    switch (variant % 5u) {
        default:
        case 0: return pulse(p * 0.5f, 0.10f) * 0.48f + sinf(6.2831853f * p * 2.0f) * 0.18f;
        case 1: return (sinf(6.2831853f * p) > 0.84f ? 1.0f : -1.0f) * 0.42f + tri(p * 3.0f) * 0.16f;
        case 2: return pulse(p * 1.25f, 0.18f) * 0.36f + sinf(6.2831853f * p * 5.0f) * 0.14f;
        case 3: return (sinf(6.2831853f * p) > sinf(6.2831853f * p * 1.5f) ? 1.0f : -1.0f) * 0.36f + tri(p * 2.5f) * 0.14f;
        case 4: return pulse(p * 2.0f, 0.08f) * 0.32f + sinf(6.2831853f * p * 6.0f) * 0.10f;
    }
}

float BytebeatEngine::eval_hybrid(uint8_t variant, uint32_t t, float ph, float carrier_hz, float morph) const {
    const float bb = eval_byte_logic(variant % 6u, t);
    const float fp = eval_float_pattern(variant % 8u, ph, carrier_hz);
    return lerp(bb, fp, 0.40f + 0.30f * morph);
}

float BytebeatEngine::eval_slot(const AlgoSlot& slot, float motion_ph, float carrier_hz, float morph) const {
    const uint32_t t = static_cast<uint32_t>(motion_ph * carrier_hz * 340.0f);
    switch (slot.kind) {
        case BYTE_LOGIC:    return eval_byte_logic(slot.variant, t);
        case FLOAT_PATTERN: return eval_float_pattern(slot.variant, motion_ph, carrier_hz);
        case FLOAT_PERC:    return eval_float_perc(slot.variant, motion_ph, carrier_hz);
        case HYBRID:        return eval_hybrid(slot.variant, t, motion_ph, carrier_hz, morph);
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

    // Full audible sweep across the whole pot travel:
    // low end = slow musical motion over audible carriers,
    // high end = audio-rate complexity.
    const float motion_hz  = lerp(0.08f, 28.0f, powf(m, 1.1f));
    const float carrier_hz = lerp(55.0f, 3200.0f, powf(m, 0.85f));

    const float motion_live = motion_hz * (0.18f + 1.00f * live) * (1.0f - 0.88f * press);
    const float carrier_live = carrier_hz * (0.55f + 0.95f * live);

    motion_phase_ += motion_live / 44100.0f;
    carrier_phase_ += carrier_live / 44100.0f;
    carrier2_phase_ += (carrier_live * (1.25f + 0.75f * morph_)) / 44100.0f;

    // sequence-like envelopes from motion phase wraps
    if (pulse(motion_phase_ * 2.0f, 0.06f) > 0.0f) env_a_ = 1.0f;
    if (pulse(motion_phase_ * 3.0f + 0.17f, 0.04f) > 0.0f) env_b_ = 1.0f;
    env_a_ *= 0.9968f;
    env_b_ *= 0.9958f;

    const float a = eval_slot(slot_a_, motion_phase_, carrier_live, morph_);
    const float b = eval_slot(slot_b_, motion_phase_ * (1.0f + 0.25f * morph_) + 0.19f * sinf(6.2831853f * carrier2_phase_), carrier_live, morph_);
    float x = lerp(a, b, morph_);

    // live color from aftertouch = immediate bright/dark tilt
    const float a_prev = eval_slot(slot_a_, motion_phase_ - (motion_live / 44100.0f), carrier_live, morph_);
    const float hp = x - 0.32f * a_prev;
    x = x * (1.0f - 0.70f * live) + hp * (0.70f * live);

    // more musical behavior as mod rises:
    // low mod => pulses and sequences
    // high mod => denser patterns / audio-rate
    const float body = sinf(6.2831853f * carrier_phase_ * 0.25f) * (0.10f + 0.22f * (1.0f - m));
    const float seq  = pulse(motion_phase_ * (2.0f + 4.0f * m), 0.18f - 0.08f * m) * (0.06f + 0.10f * (1.0f - m));
    const float air  = sinf(6.2831853f * carrier2_phase_) * (0.02f + 0.15f * m);
    x += body + seq + air;

    // percussive structure to keep floatbeats from becoming only FM hiss
    x += env_a_ * (0.18f + 0.14f * (1.0f - m));
    x += env_b_ * (0.12f + 0.10f * (1.0f - m));

    const float drive = 0.76f + 0.38f * live + 0.22f * press + 0.30f * m;
    x *= drive;

    x *= 0.72f;
    return softclip(x);
}

}  // namespace synth
