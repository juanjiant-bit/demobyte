#pragma once
// bytebeat_graph.h — BYT3 parametric bytebeat engine
//
// Mantiene la API pública histórica para no romper StateManager/Glide,
// pero la implementación interna ya no usa árboles aleatorios. En su lugar,
// usa un motor bytebeat determinista con:
//   - banco curado de 17 fórmulas
//   - morph A/B
//   - tiempo procesado (rate/phase/jitter/seed)
//   - post-shaping (mask/shift/xor-fold/feedback)
//   - filtro macro LP ↔ clean ↔ HP
//   - envelope macro + dc blocker
//
// generate(seed, zone, cfg) ahora deriva un patch estable a partir de seed+zone.
// evaluate(ctx) renderiza el sample en tiempo real usando el contexto live.
#include "bytebeat_node.h"
#include "zone_config.h"
#include <cstdint>

class BytebeatGraph {
public:
    static constexpr uint8_t FORMULA_COUNT = 17;

    void    generate(uint32_t seed, uint8_t zone, const ZoneConfig& cfg);
    int16_t evaluate(const EvalContext& ctx);
    int16_t preview_evaluate(const EvalContext& ctx) const;

    uint32_t get_seed()          const { return seed_; }
    uint8_t  get_zone()          const { return zone_; }
    float    get_quality_score() const { return quality_score_; }
    void     debug_print()       const;

    // Live parameter control for StateManager / sequencer / snapshots.
    uint8_t get_formula_a() const { return params_.formula_a; }
    uint8_t get_formula_b() const { return params_.formula_b; }
    uint8_t get_morph() const { return params_.morph; }
    uint8_t get_rate() const { return params_.rate; }
    uint8_t get_shift() const { return params_.shift; }
    uint8_t get_mask() const { return params_.mask; }
    uint8_t get_feedback() const { return params_.feedback; }
    uint8_t get_jitter() const { return params_.jitter; }
    uint8_t get_phase() const { return params_.phase; }
    uint8_t get_xor_fold() const { return params_.xor_fold; }
    uint8_t get_seed_mod() const { return params_.seed; }
    uint8_t get_filter_macro() const { return params_.filter_macro; }
    uint8_t get_resonance() const { return params_.resonance; }
    uint8_t get_env_macro() const { return params_.env_macro; }

    void set_formula_a(uint8_t v) { params_.formula_a = v % FORMULA_COUNT; }
    void set_formula_b(uint8_t v) { params_.formula_b = v % FORMULA_COUNT; }
    void set_morph(uint8_t v) { params_.morph = v; }
    void set_rate(uint8_t v) { params_.rate = v; }
    void set_shift(uint8_t v) { params_.shift = v & 7u; }
    void set_mask(uint8_t v) { params_.mask = v; }
    void set_feedback(uint8_t v) { params_.feedback = v; }
    void set_jitter(uint8_t v) { params_.jitter = v; }
    void set_phase(uint8_t v) { params_.phase = v; }
    void set_xor_fold(uint8_t v) { params_.xor_fold = v; }
    void set_seed_mod(uint8_t v) { params_.seed = v; }
    void set_filter_macro(uint8_t v) { params_.filter_macro = v; }
    void set_resonance(uint8_t v) { params_.resonance = v; }
    void set_env_macro(uint8_t v) { params_.env_macro = v; }

private:
    struct EngineParams {
        uint8_t formula_a = 0;
        uint8_t formula_b = 1;
        uint8_t morph = 96;
        uint8_t rate = 128;
        uint8_t shift = 2;
        uint8_t mask = 160;
        uint8_t feedback = 24;
        uint8_t jitter = 12;
        uint8_t phase = 32;
        uint8_t xor_fold = 0;
        uint8_t seed = 0;
        uint8_t filter_macro = 128;
        uint8_t resonance = 24;
        uint8_t env_macro = 128;
    };

    static constexpr uint32_t SILENCE_THRESHOLD = 2205;

    static uint32_t lcg_next(uint32_t& s);
    static uint8_t  lcg_u8(uint32_t& s);
    static uint8_t  eval_formula(uint8_t id, uint32_t t, uint8_t seed, uint8_t shift);
    static uint8_t  apply_mask_family(uint8_t x, uint8_t mask);
    static int16_t  soft_clip_16(int32_t x);
    static uint16_t rate_from_u8(uint8_t v);
    static int32_t  phase_from_u8(uint8_t v);
    static uint8_t  zone_formula_pick(uint8_t zone, uint32_t& rng, bool secondary);

    uint8_t  compute_env_gain_u8(const EvalContext& ctx) const;
    uint32_t make_time_a(uint32_t t, const EvalContext& ctx, uint8_t env_gain) const;
    uint32_t make_time_b(uint32_t t, const EvalContext& ctx, uint8_t env_gain) const;
    int16_t  apply_filter_macro(int16_t x, const EvalContext& ctx, uint8_t filter_macro, uint8_t resonance);
    int16_t  apply_env_macro(int16_t x, const EvalContext& ctx);
    int16_t  apply_feedback_and_color(uint8_t raw, const EvalContext& ctx, uint8_t morph, uint8_t mask, uint8_t feedback);
    int16_t  apply_anti_alias_lp(int16_t x, uint8_t rate, uint8_t shift);
    void     update_smoothed_params(const EvalContext& ctx);
    uint8_t  render_formula_pair(uint8_t active_a, uint8_t active_b, uint32_t t_a, uint32_t t_b, uint8_t shift, uint8_t morph) const;

    EngineParams params_ = {};

    uint32_t seed_ = 0;
    uint8_t  zone_ = 0;
    float    quality_score_ = 0.0f;

    // Runtime state.
    int32_t  fb_state_ = 0;
    int32_t  lp_state_ = 0;
    int32_t  aa_lp_state_ = 0;
    int32_t  dc_prev_x_ = 0;
    int32_t  dc_prev_y_ = 0;
    uint32_t silence_count_ = 0;
    uint16_t noise_state_ = 0xACE1u;

    // Smoothed/live runtime values for destructive params. 8.8 fixed point where noted.
    int32_t  morph_smooth_ = 96 << 8;
    int32_t  feedback_smooth_ = 24 << 8;
    int32_t  phase_smooth_ = 32 << 8;
    int32_t  filter_macro_smooth_ = 128 << 8;
    int32_t  resonance_smooth_ = 24 << 8;
    int32_t  rate_smooth_ = 128 << 8;
    int32_t  shift_smooth_ = 2 << 8;
    int32_t  mask_smooth_ = 160 << 8;

    uint8_t  active_formula_a_ = 0;
    uint8_t  active_formula_b_ = 1;
    uint8_t  prev_formula_a_ = 0;
    uint8_t  prev_formula_b_ = 1;
    uint8_t  formula_fade_a_ = 255;
    uint8_t  formula_fade_b_ = 255;

    int32_t  level_env_ = 0;
    int32_t  level_gain_ = 256;

    // v26: mod matrix interna barata en CPU.
    int32_t  mod_mask_smooth_ = 0;
    int32_t  mod_morph_smooth_ = 0;
    int32_t  mod_rate_smooth_ = 0;
};
