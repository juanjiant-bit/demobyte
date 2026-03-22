#include "bytebeat_graph.h"
#include "../utils/debug_log.h"

namespace {
constexpr uint8_t kFormulaFamilies[5][6] = {
    {0, 1, 2, 10, 11, 15},   // melodic
    {0, 1, 2, 3, 10, 15},    // semi-melodic
    {1, 3, 4, 6, 10, 12},    // neutral
    {4, 5, 7, 13, 14, 16},   // semi-chaos
    {5, 7, 8, 9, 14, 16},    // chaos
};
constexpr uint16_t kRateTable[16] = {
    1, 2, 3, 4, 6, 8, 12, 16,
    24, 32, 48, 64, 96, 128, 192, 256
};
constexpr uint8_t kFormulaGain[BytebeatGraph::FORMULA_COUNT] = {
    212, 210, 206, 214,
    202, 206, 200, 196,
    190, 188, 184, 192,
    180, 186, 184, 208,
    200
};
inline uint8_t clamp_u8(int32_t v) {
    return (v < 0) ? 0 : (v > 255 ? 255 : (uint8_t)v);
}
inline int16_t clamp_i16(int32_t v) {
    return (v < -32768) ? -32768 : (v > 32767 ? 32767 : (int16_t)v);
}

enum FormulaType : uint8_t {
    T_MELODY = 0,
    BIT_HARMONY,
    PERCUSSIVE,
    CHAOS,
    HYBRID
};

enum MorphMode : uint8_t {
    MORPH_LINEAR = 0,
    MORPH_CROSS,
    MORPH_SAFE
};

inline FormulaType get_formula_type(uint8_t id) {
    const uint8_t f = id % BytebeatGraph::FORMULA_COUNT;
    if (f <= 3u)  return T_MELODY;
    if (f <= 7u)  return BIT_HARMONY;
    if (f <= 11u) return PERCUSSIVE;
    if (f <= 14u) return CHAOS;
    return HYBRID;
}

inline MorphMode get_morph_mode(FormulaType a, FormulaType b) {
    if (a == b) return MORPH_LINEAR;

    const bool a_melodic = (a == T_MELODY);
    const bool b_melodic = (b == T_MELODY);
    const bool a_harm = (a == BIT_HARMONY);
    const bool b_harm = (b == BIT_HARMONY);
    const bool a_perc = (a == PERCUSSIVE);
    const bool b_perc = (b == PERCUSSIVE);
    const bool a_hybrid = (a == HYBRID);
    const bool b_hybrid = (b == HYBRID);
    const bool a_chaos = (a == CHAOS);
    const bool b_chaos = (b == CHAOS);

    if ((a_melodic and b_hybrid) || (a_hybrid and b_melodic) ||
        (a_harm and b_hybrid)    || (a_hybrid and b_harm) ||
        (a_perc and b_perc)) {
        return MORPH_CROSS;
    }

    if ((a_chaos and b_hybrid) || (a_hybrid and b_chaos)) {
        return MORPH_CROSS;
    }

    return MORPH_SAFE;
}

inline uint8_t morph_mix_family_aware(uint8_t a, uint8_t b, uint8_t m, FormulaType ta, FormulaType tb) {
    switch (get_morph_mode(ta, tb)) {
        case MORPH_LINEAR: {
            const uint16_t mix = (uint16_t)a * (uint16_t)(255u - m) + (uint16_t)b * (uint16_t)m;
            return (uint8_t)(mix >> 8);
        }

        case MORPH_CROSS: {
            const uint16_t xa = ((uint16_t)a * (uint16_t)(255u - m)) >> 8;
            const uint16_t xb = ((uint16_t)b * (uint16_t)m) >> 8;
            return (uint8_t)((xa + xb) > 255u ? 255u : (xa + xb));
        }

        case MORPH_SAFE:
        default: {
            constexpr uint8_t kFadeZone = 32u;
            if (m < (uint8_t)(128u - kFadeZone)) return a;
            if (m > (uint8_t)(128u + kFadeZone)) return b;

            const uint8_t local = (uint8_t)((uint16_t)(m - (uint8_t)(128u - kFadeZone)) * 4u);
            const uint16_t xa = ((uint16_t)a * (uint16_t)(255u - local)) >> 8;
            const uint16_t xb = ((uint16_t)b * (uint16_t)local) >> 8;
            return (uint8_t)((xa + xb) > 255u ? 255u : (xa + xb));
        }
    }
}

} // namespace

uint32_t BytebeatGraph::lcg_next(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return s;
}

uint8_t BytebeatGraph::lcg_u8(uint32_t& s) {
    return (uint8_t)(lcg_next(s) >> 24);
}

uint8_t BytebeatGraph::zone_formula_pick(uint8_t zone, uint32_t& rng, bool secondary) {
    const uint8_t z = (zone < 5) ? zone : 4;
    const uint8_t idx = (uint8_t)(lcg_next(rng) % 6u);
    uint8_t id = kFormulaFamilies[z][idx];
    if (secondary && ((lcg_next(rng) & 3u) == 0u)) {
        id = (uint8_t)((id + 1u + (lcg_next(rng) % 3u)) % FORMULA_COUNT);
    }
    return id;
}

uint16_t BytebeatGraph::rate_from_u8(uint8_t v) {
    const uint8_t seg = v >> 4;
    const uint8_t frac = v & 0x0F;
    const uint16_t a = kRateTable[seg];
    const uint16_t b = kRateTable[(seg < 15) ? (seg + 1) : 15];
    return (uint16_t)(a + (((int32_t)(b - a) * frac) >> 4));
}

int32_t BytebeatGraph::phase_from_u8(uint8_t v) {
    // v12: limitar el rango a offsets temporales más musicales y menos destructivos.
    return (((int32_t)v - 128) * 64) >> 1;
}

uint8_t BytebeatGraph::eval_formula(uint8_t id, uint32_t t, uint8_t seed, uint8_t shift) {
    const uint32_t s = 1u + (uint32_t)(seed & 7u);
    const uint32_t sh = (uint32_t)(shift & 7u);
    const uint32_t mask = 0xFFu >> (sh & 3u);
    switch (id % FORMULA_COUNT) {
        // T_MELODY
        case 0:  return (uint8_t)(t * ((((t >> 10) & 42u) & mask) ? (((t >> 10) & 42u) & mask) : 1u));
        case 1:  return (uint8_t)(t * ((((t >> 9) ^ (t >> 11)) & 28u) + 4u));
        case 2:  return (uint8_t)(t * ((((t >> 8) & 15u) ^ ((t >> 11) & 7u)) + 3u));
        case 3:  return (uint8_t)(t * ((((t >> 10) & 5u) | ((t >> 13) & 2u)) + 2u));

        // BIT_HARMONY
        case 4:  return (uint8_t)(t & (t >> 8));
        case 5:  return (uint8_t)(((t * 5u) & (t >> 7)) | ((t * 3u) & (t >> 10)));
        case 6:  return (uint8_t)(((t >> 6) | (t * 3u)) & ((t >> 9) | (t * 5u)));
        case 7:  return (uint8_t)(((t >> 5) & (t >> 8)) | ((t >> 3) & (t * 2u)));

        // PERCUSSIVE
        case 8:  return (uint8_t)(((t >> 4) & (t >> 7)) * ((255u - (t >> 6)) & 255u));
        case 9:  return (uint8_t)(((t * (9u + (s & 1u))) & (t >> 4)) ^ ((t * (5u + ((s >> 1) & 1u))) & (t >> 7)));
        case 10: return (uint8_t)((t >> 2) ^ (t >> 5) ^ (t >> 7));
        case 11: return (uint8_t)((t * ((t >> 9) & 3u)) & (t >> 5));

        // CHAOS
        case 12: return (uint8_t)(t ^ (t >> 3) ^ (t >> 6));
        case 13: return (uint8_t)((t * (t >> 9)) ^ (t >> 7) ^ (t >> 13));
        case 14: return (uint8_t)(((t * 7u) & (t >> 9)) ^ ((t * 11u) & (t >> 11)));

        // HYBRID
        case 15: return (uint8_t)((t * ((((t >> 10) & 21u) + 3u))) | ((t >> 7) & (t >> 9)));
        case 16: return (uint8_t)(((t * ((((t >> 11) & 13u) + 2u))) ^ ((t * 5u) & (t >> 8))));
        default: return (uint8_t)t;
    }
}

uint8_t BytebeatGraph::apply_mask_family(uint8_t x, uint8_t mask) {
    // v25: 8 familias de máscara más musicales y previsibles.
    // Cada familia ocupa 32 pasos y mantiene una identidad clara a lo largo del barrido.
    const uint8_t family = mask >> 5;   // 0..7
    const uint8_t amt = mask & 31u;     // 0..31

    switch (family) {
        case 0: {
            // Clean -> progressive bit depth reduction.
            const uint8_t steps = amt >> 2; // 0..7
            if (steps == 0) return x;
            const uint8_t keep = (uint8_t)(8u - steps);
            const uint8_t qmask = (keep >= 8u) ? 0xFFu : (uint8_t)(0xFFu << (8u - keep));
            return (uint8_t)(x & qmask);
        }
        case 1: {
            // Pulse ladder: enfatiza nibbles altos y abre detalle bajo gradualmente.
            static const uint8_t kMasks[8] = {0x80u, 0xC0u, 0xE0u, 0xF0u, 0xF8u, 0xFCu, 0xFEu, 0xFFu};
            return (uint8_t)(x & kMasks[amt >> 2]);
        }
        case 2: {
            // Alternating gates: PWM digital con interpolación de patrones.
            static const uint8_t kMasks[8] = {0x88u, 0xAAu, 0xCCu, 0xEEu, 0x99u, 0xBBu, 0xDDu, 0xFFu};
            const uint8_t m = kMasks[amt >> 2];
            return (uint8_t)((x & m) | ((x >> 1) & (uint8_t)(~m)));
        }
        case 3: {
            // Staircase crusher: mezcla coarse quantize con recovery gradual.
            const uint8_t coarse = (uint8_t)(x & 0xE0u);
            const uint8_t detail = (uint8_t)((x >> (3u - ((amt >> 4) & 1u))) & 0x1Fu);
            const uint8_t blend = (uint8_t)(amt << 1);
            return (uint8_t)(coarse | (detail & blend));
        }
        case 4: {
            // Mirror/fold masks: aporta armónicos sin destruir transientes.
            const uint8_t mirrored = (uint8_t)(((x & 0xF0u) >> 4) | ((x & 0x0Fu) << 4));
            const uint8_t t = amt << 3;
            return (uint8_t)((((uint16_t)x * (255u - t)) + ((uint16_t)mirrored * t)) >> 8);
        }
        case 5: {
            // Rhythmic comb: deja huecos regulares y recupera parciales.
            static const uint8_t kMasks[8] = {0x81u, 0xC3u, 0xA5u, 0xE7u, 0x99u, 0xDBu, 0xBDu, 0xFFu};
            const uint8_t m = kMasks[amt >> 2];
            return (uint8_t)((x & m) | ((x >> 2) & (uint8_t)(~m)));
        }
        case 6: {
            // Harmonic XOR color: coloración progresiva, no solo destrucción binaria.
            const uint8_t color = (uint8_t)(0x11u * (1u + (amt >> 3))); // 0x11..0x44
            const uint8_t y = (uint8_t)(x ^ color);
            const uint8_t t = amt << 3;
            return (uint8_t)((((uint16_t)x * (255u - t)) + ((uint16_t)y * t)) >> 8);
        }
        case 7: {
            // Open family: casi clean, con small shimmer/tilt al final del recorrido.
            const uint8_t shimmer = (uint8_t)((x & 0xFCu) | ((x + (amt >> 1)) & 0x03u));
            const uint8_t t = amt << 2;
            return (uint8_t)((((uint16_t)x * (255u - t)) + ((uint16_t)shimmer * t)) >> 8);
        }
        default:
            return x;
    }
}

int16_t BytebeatGraph::soft_clip_16(int32_t x) {
    x = (x > 32767) ? 32767 : (x < -32768 ? -32768 : x);
    int32_t ax = (x < 0) ? -x : x;
    // Soft clip muy barato y más musical que el clamp duro.
    ax -= (ax * ax) >> 18;
    if (ax > 30000) ax = 30000;
    return (int16_t)((x < 0) ? -ax : ax);
}

void BytebeatGraph::generate(uint32_t seed, uint8_t zone, const ZoneConfig& cfg) {
    (void)cfg;
    seed_ = seed;
    zone_ = zone;
    uint32_t rng = seed ^ (uint32_t(zone) * 0x9E3779B9u);

    params_.formula_a = zone_formula_pick(zone, rng, false);
    params_.formula_b = zone_formula_pick(zone, rng, true);
    if (params_.formula_a == params_.formula_b) {
        params_.formula_b = (uint8_t)((params_.formula_b + 3u + (rng & 3u)) % FORMULA_COUNT);
    }

    params_.morph        = (uint8_t)(48u + (lcg_u8(rng) & 0x9Fu));
    params_.rate         = (uint8_t)(64u + (lcg_u8(rng) & 0xBFu));
    params_.shift        = (uint8_t)(1u + (lcg_u8(rng) & 7u));
    params_.mask         = lcg_u8(rng);
    params_.feedback     = (uint8_t)((zone < 2) ? (lcg_u8(rng) & 63u) : (32u + (lcg_u8(rng) & 95u)));
    params_.jitter       = (uint8_t)((zone == 0) ? (lcg_u8(rng) & 15u) : (8u + (lcg_u8(rng) & 31u)));
    params_.phase        = lcg_u8(rng);
    params_.xor_fold     = lcg_u8(rng);
    params_.seed         = lcg_u8(rng);
    params_.filter_macro = (uint8_t)(96u + (lcg_u8(rng) & 0x7Fu));
    params_.resonance    = (uint8_t)(16u + (lcg_u8(rng) & 63u));
    params_.env_macro    = lcg_u8(rng);

    fb_state_ = 0;
    lp_state_ = 0;
    aa_lp_state_ = 0;
    dc_prev_x_ = 0;
    dc_prev_y_ = 0;
    silence_count_ = 0;
    morph_smooth_ = ((int32_t)params_.morph) << 8;
    feedback_smooth_ = ((int32_t)params_.feedback) << 8;
    phase_smooth_ = ((int32_t)params_.phase) << 8;
    filter_macro_smooth_ = ((int32_t)params_.filter_macro) << 8;
    resonance_smooth_ = ((int32_t)params_.resonance) << 8;
    rate_smooth_ = ((int32_t)params_.rate) << 8;
    shift_smooth_ = ((int32_t)params_.shift) << 8;
    mask_smooth_ = ((int32_t)params_.mask) << 8;
    active_formula_a_ = prev_formula_a_ = params_.formula_a;
    active_formula_b_ = prev_formula_b_ = params_.formula_b;
    formula_fade_a_ = 255;
    formula_fade_b_ = 255;
    level_env_ = 0;
    level_gain_ = 256;
    quality_score_ = 0.92f;

    LOG("BB: seed=%lu zone=%u A=%u B=%u morph=%u rate=%u shift=%u mask=%u fb=%u",
        (unsigned long)seed_, zone_, params_.formula_a, params_.formula_b,
        params_.morph, params_.rate, params_.shift, params_.mask, params_.feedback);
}


uint8_t BytebeatGraph::compute_env_gain_u8(const EvalContext& ctx) const {
    const uint8_t env = (uint8_t)(params_.env_macro + (uint8_t)(ctx.macro * 64.0f));
    const uint32_t period = 256u + ((uint32_t)(255u - env) << 4);
    const uint32_t ph = (ctx.t + ((uint32_t)params_.seed << 3)) % period;
    uint16_t gain = 255u;

    if (env < 51u) {
        gain = (uint16_t)((ph * 255u) / period);
    } else if (env < 102u) {
        gain = (ph < 24u) ? 255u : (uint16_t)(255u - ((ph - 24u) * 220u) / (period - 24u));
    } else if (env < 153u) {
        gain = (ph < (period >> 1)) ? 255u : 0u;
    } else if (env < 204u) {
        gain = (ph < 20u) ? 255u : 180u;
    } else {
        gain = (uint16_t)(96u + ((period - ph) * 159u) / period);
    }

    if (ctx.note_mode_active) {
        gain = (uint16_t)((gain * (200u + (uint16_t)(ctx.note_pitch_ratio * 28.0f))) >> 8);
        if (gain > 255u) gain = 255u;
    }
    return (uint8_t)gain;
}

uint32_t BytebeatGraph::make_time_a(uint32_t t, const EvalContext& ctx, uint8_t env_gain) const {
    const uint16_t base_rate = rate_from_u8((uint8_t)(rate_smooth_ >> 8));
    const uint16_t td = (ctx.time_div <= 0.30f) ? 64u
                      : (ctx.time_div <= 0.75f) ? 96u
                      : (ctx.time_div <= 1.50f) ? 128u
                      : (ctx.time_div <= 3.00f) ? 192u : 255u;
    const uint32_t scaled = (uint32_t)(((uint64_t)t * base_rate * td) >> 8);

    const uint32_t tonal_bump = (uint32_t)(ctx.note_mode_active ? (ctx.note_pitch_ratio * 64.0f) : 0.0f);
    const uint32_t seed_skew = ((uint32_t)params_.seed << 7) ^ ((uint32_t)zone_ << 13);

    // Micro-drift temporal: apenas mueve el clock cuando jitter está presente.
    uint32_t drift = 0;
    const uint8_t drift_amt = (uint8_t)(params_.jitter >> 4);
    if (drift_amt > 0) {
        const uint32_t n = ((t + 0x9E3779B9u) ^ ((uint32_t)params_.seed << 9)) * 747796405u + 2891336453u;
        drift = (n >> 29) % (uint32_t)(drift_amt + 1u);
    }
    return scaled + seed_skew + tonal_bump + drift;
}

uint32_t BytebeatGraph::make_time_b(uint32_t t, const EvalContext& ctx, uint8_t env_gain) const {
    uint32_t tb = make_time_a(t, ctx, env_gain);
    const uint8_t phase_u8 = clamp_u8((phase_smooth_ >> 8) + (int32_t)(ctx.spread * 48.0f));
    tb += (uint32_t)phase_from_u8(phase_u8);

    // v12: jitter más corto y más parecido a micro-variación de clock.
    const uint8_t jitter_src = clamp_u8((int32_t)params_.jitter + (int32_t)(ctx.spread * 24.0f));
    const uint8_t jitter_amt = (uint8_t)(jitter_src >> 2);
    if (jitter_amt > 0) {
        const uint32_t n = ((t ^ (uint32_t)params_.seed * 257u) * 1103515245u + 12345u);
        const int32_t bipolar = (int32_t)((n >> 24) & 0xFFu) - 128;
        tb += (uint32_t)((bipolar * jitter_amt) >> 1);
    }
    return tb;
}

int16_t BytebeatGraph::apply_feedback_and_color(uint8_t raw, const EvalContext& ctx, uint8_t morph, uint8_t mask, uint8_t feedback) {
    const uint8_t shaped = apply_mask_family(
        raw,
        (uint8_t)(mask + (uint8_t)(ctx.tonal * 64.0f))
    );

    int32_t x = ((int32_t)shaped - 128) << 8;

    if (params_.xor_fold >= 86u && params_.xor_fold < 171u) {
        const uint8_t fb_byte = clamp_u8((fb_state_ >> 8) + 128);
        const int32_t xor_x = (((int32_t)((uint8_t)raw ^ fb_byte) - 128) << 8);
        x = (x + xor_x) >> 1;
    } else if (params_.xor_fold >= 171u) {
        int32_t folded = x;
        if (folded > 32767) folded = 65535 - folded;
        if (folded < -32768) folded = -65535 - folded;
        x = clamp_i16(folded);
    }

    const int32_t fb_target = x - fb_state_;
    fb_state_ += (fb_target * 32) >> 8;  // feedback filtrado más musical

    // v12: limitar el rango destructivo del feedback.
    const int32_t fb_amt = (((int32_t)feedback * 192) >> 8) + (int32_t)(ctx.spread * 12.0f);
    x += (fb_state_ * fb_amt) >> 8;

    const int32_t color_gain = 192 + (((int32_t)morph + (int32_t)(ctx.macro * 64.0f)) >> 1);
    x = (x * color_gain) >> 8;

    if (x > 32767) x = 32767;
    if (x < -32768) x = -32768;
    return soft_clip_16(x);
}

int16_t BytebeatGraph::apply_env_macro(int16_t x, const EvalContext& ctx) {
    const uint8_t gain = compute_env_gain_u8(ctx);
    return (int16_t)((x * (int32_t)gain) >> 8);
}

int16_t BytebeatGraph::apply_filter_macro(int16_t x, const EvalContext& ctx, uint8_t filter_macro, uint8_t resonance) {
    uint8_t macro = filter_macro;
    macro = (uint8_t)(macro + (uint8_t)(ctx.tonal * 96.0f));
    resonance = (uint8_t)(resonance + (uint8_t)(ctx.spread * 48.0f));

    int32_t inp = x + ((lp_state_ * resonance) >> 10);
    uint8_t coeff = 10u + ((macro < 128u ? macro : 255u - macro) >> 3);
    lp_state_ += (inp - lp_state_) >> ((coeff > 31u) ? 5u : 4u);

    const int16_t lp = clamp_i16(lp_state_);
    const int16_t hp = clamp_i16((int32_t)x - lp_state_);

    if (macro < 112u) {
        return (int16_t)((((int32_t)lp * (112 - macro)) + ((int32_t)x * macro)) / 112);
    }
    if (macro > 144u) {
        const uint8_t m = (uint8_t)(macro - 144u);
        return (int16_t)((((int32_t)x * (111 - m)) + ((int32_t)hp * m)) / 111);
    }
    return x;
}


int16_t BytebeatGraph::apply_anti_alias_lp(int16_t x, uint8_t rate, uint8_t shift) {
    // 1-pole lowpass muy barato para recortar aliasing en regiones agresivas.
    uint8_t pole = 2u + (rate >> 6);
    pole += (shift < 2u) ? 2u : (shift < 4u ? 1u : 0u);
    if (pole > 6u) pole = 6u;
    aa_lp_state_ += (((int32_t)x << 8) - aa_lp_state_) >> pole;
    return (int16_t)(aa_lp_state_ >> 8);
}

void BytebeatGraph::update_smoothed_params(const EvalContext& ctx) {
    (void)ctx;
    auto slew = [](int32_t& current, int32_t target, uint8_t shift) {
        current += (target - current) >> shift;
    };

    slew(morph_smooth_, ((int32_t)params_.morph) << 8, 3);
    slew(feedback_smooth_, ((int32_t)params_.feedback) << 8, 4);
    slew(phase_smooth_, ((int32_t)params_.phase) << 8, 3);
    slew(filter_macro_smooth_, ((int32_t)params_.filter_macro) << 8, 3);
    slew(resonance_smooth_, ((int32_t)params_.resonance) << 8, 4);
    slew(rate_smooth_, ((int32_t)params_.rate) << 8, 3);
    slew(shift_smooth_, ((int32_t)params_.shift) << 8, 4);
    slew(mask_smooth_, ((int32_t)params_.mask) << 8, 4);

    if (params_.formula_a != active_formula_a_) {
        prev_formula_a_ = active_formula_a_;
        active_formula_a_ = params_.formula_a % FORMULA_COUNT;
        formula_fade_a_ = 0;
    }
    if (params_.formula_b != active_formula_b_) {
        prev_formula_b_ = active_formula_b_;
        active_formula_b_ = params_.formula_b % FORMULA_COUNT;
        formula_fade_b_ = 0;
    }

    if (formula_fade_a_ < 255u) {
        const uint16_t next = (uint16_t)formula_fade_a_ + 24u;
        formula_fade_a_ = (next > 255u) ? 255u : (uint8_t)next;
    }
    if (formula_fade_b_ < 255u) {
        const uint16_t next = (uint16_t)formula_fade_b_ + 24u;
        formula_fade_b_ = (next > 255u) ? 255u : (uint8_t)next;
    }
}

uint8_t BytebeatGraph::render_formula_pair(uint8_t active_a, uint8_t active_b, uint32_t t_a, uint32_t t_b, uint8_t shift, uint8_t morph) const {
    const uint8_t seed_b = (uint8_t)(params_.seed ^ 0x5Au);
    uint8_t a = eval_formula(active_a, t_a, params_.seed, shift);
    uint8_t b = eval_formula(active_b, t_b, seed_b, (uint8_t)(shift + 1u));

    a = (uint8_t)(((uint16_t)a * kFormulaGain[active_a % FORMULA_COUNT]) >> 8);
    b = (uint8_t)(((uint16_t)b * kFormulaGain[active_b % FORMULA_COUNT]) >> 8);

    if (formula_fade_a_ < 255u) {
        uint8_t old_a = eval_formula(prev_formula_a_, t_a, params_.seed, shift);
        old_a = (uint8_t)(((uint16_t)old_a * kFormulaGain[prev_formula_a_ % FORMULA_COUNT]) >> 8);
        a = (uint8_t)((((uint16_t)old_a * (255u - formula_fade_a_)) + ((uint16_t)a * formula_fade_a_)) >> 8);
    }
    if (formula_fade_b_ < 255u) {
        uint8_t old_b = eval_formula(prev_formula_b_, t_b, seed_b, (uint8_t)(shift + 1u));
        old_b = (uint8_t)(((uint16_t)old_b * kFormulaGain[prev_formula_b_ % FORMULA_COUNT]) >> 8);
        b = (uint8_t)((((uint16_t)old_b * (255u - formula_fade_b_)) + ((uint16_t)b * formula_fade_b_)) >> 8);
    }

    const FormulaType type_a = get_formula_type(active_a);
    const FormulaType type_b = get_formula_type(active_b);
    return morph_mix_family_aware(a, b, morph, type_a, type_b);
}


int16_t BytebeatGraph::preview_evaluate(const EvalContext& ctx) const {
    BytebeatGraph temp = *this;
    return temp.evaluate(ctx);
}

int16_t BytebeatGraph::evaluate(const EvalContext& ctx) {
    update_smoothed_params(ctx);

    const uint8_t env_gain = compute_env_gain_u8(ctx);
    const int32_t fb_abs = (fb_state_ < 0) ? -fb_state_ : fb_state_;

    // v26 mod matrix: jitter -> mask (micro color movement).
    const int32_t jitter_centered = (int32_t)params_.jitter - 128;
    const int32_t jitter_lfo = (((int32_t)((ctx.t >> 7) + ((uint32_t)params_.seed << 1)) & 31) - 16);
    const int32_t mask_mod_target = (jitter_centered * jitter_lfo) >> 4;
    mod_mask_smooth_ += (mask_mod_target - mod_mask_smooth_) >> 3;
    const uint8_t mod_mask = clamp_u8((mask_smooth_ >> 8) + (mod_mask_smooth_ >> 1));

    // v26 mod matrix: feedback energy -> morph.
    const int32_t fb_norm = fb_abs >> 7;
    const int32_t morph_mod_target = ((fb_norm - 64) * (int32_t)((params_.feedback >> 3) + 4)) >> 4;
    mod_morph_smooth_ += (morph_mod_target - mod_morph_smooth_) >> 4;

    uint16_t morph = clamp_u8((int32_t)(morph_smooth_ >> 8) + (int32_t)(ctx.macro * 127.0f) - 32 + (mod_morph_smooth_ >> 1));
    // v12: morph con curva S simple (smoothstep en entero 0..255).
    const uint16_t m = morph;
    morph = (uint16_t)(((m * m) * (uint32_t)(765 - (m << 1))) >> 16);

    const uint32_t t_a = make_time_a(ctx.t, ctx, env_gain);
    const uint32_t t_b = make_time_b(ctx.t, ctx, env_gain);

    const uint8_t shift = (uint8_t)(shift_smooth_ >> 8);
    const uint8_t raw = render_formula_pair(active_formula_a_, active_formula_b_, t_a, t_b, shift, (uint8_t)morph);

    int16_t out = apply_feedback_and_color(raw, ctx, (uint8_t)morph, mod_mask, (uint8_t)(feedback_smooth_ >> 8));
    out = apply_anti_alias_lp(out, (uint8_t)(rate_smooth_ >> 8), shift);
    out = apply_filter_macro(out, ctx, (uint8_t)(filter_macro_smooth_ >> 8), (uint8_t)(resonance_smooth_ >> 8));
    out = (int16_t)((out * (int32_t)env_gain) >> 8);

    // DC blocker simple y barato.
    const int32_t y = out - dc_prev_x_ + ((dc_prev_y_ * 252) >> 8);
    dc_prev_x_ = out;
    dc_prev_y_ = y;
    out = clamp_i16(y);

    // Normalización/trim muy suave para emparejar fórmulas y morphs.
    const int32_t abs_out = (out < 0) ? -out : out;
    level_env_ += (abs_out - level_env_) >> 6;
    const int32_t desired_gain = (level_env_ > 256) ? ((12000 << 8) / level_env_) : (256 << 0);
    const int32_t clamped_gain = (desired_gain < 160) ? 160 : (desired_gain > 416 ? 416 : desired_gain);
    level_gain_ += (clamped_gain - level_gain_) >> 5;
    out = clamp_i16((out * level_gain_) >> 8);

    out = soft_clip_16(out);

    if (out > -64 && out < 64) {
        if (++silence_count_ > SILENCE_THRESHOLD) {
            noise_state_ ^= (uint16_t)(noise_state_ << 7);
            noise_state_ ^= (uint16_t)(noise_state_ >> 9);
            noise_state_ ^= (uint16_t)(noise_state_ << 8);
            out = (int16_t)(((int16_t)(noise_state_ & 0x3FF) - 512) << 4);
            if (silence_count_ > SILENCE_THRESHOLD + 2205u) silence_count_ = 0;
        }
    } else {
        silence_count_ = 0;
    }

    return out;
}

void BytebeatGraph::debug_print() const {
    LOG("BB engine [family-aware morph]: seed=%lu zone=%u A=%u B=%u morph=%u rate=%u shift=%u mask=%u fb=%u jitter=%u phase=%u xor_fold=%u filt=%u env=%u score=%.3f",
        (unsigned long)seed_, zone_, params_.formula_a, params_.formula_b,
        params_.morph, params_.rate, params_.shift, params_.mask,
        params_.feedback, params_.jitter, params_.phase, params_.xor_fold,
        params_.filter_macro, params_.env_macro, (double)quality_score_);
}
