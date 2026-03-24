#include "../synth/quantizer.h"
// state_manager.cpp — Bytebeat Machine V1.21
// V1.21: save path alineado a NUM_POTS=7 y snapshots live coherentes
// V1.19: delay_div/wet en Snapshot (persiste en flash)
// Cambios V1.7:
//   + fill_context() exporta drum_color/decay, note_mode_active, note_pitch_ratio
//   + do_trigger() interpola drum params (glide entre snapshots)
#include "state_manager.h"
#include "flash_store.h"
#include "../audio/audio_engine.h"
#include "../utils/debug_log.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/sync.h"

namespace {
inline float clampf01_local(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

inline float jitter_value(float current, float amount, float span, float r01) {
    const float delta = (r01 * 2.0f - 1.0f) * span * amount;
    return clampf01_local(current + delta);
}

inline float biased_jitter_value(float current, float amount, float span, float r01) {
    const float signed_bias = (r01 * 2.0f - 1.0f);
    const float shaped = signed_bias * signed_bias * signed_bias;
    return clampf01_local(current + shaped * span * amount);
}

inline float jitter_in_range(float current, float amount, float span, float lo, float hi, float r01) {
    const float signed_bias = (r01 * 2.0f - 1.0f);
    const float shaped = signed_bias * signed_bias * signed_bias;
    float v = current + shaped * span * amount;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

inline uint8_t quantized_time_div_index(float td) {
    uint8_t best = 0;
    float best_err = 9999.0f;
    for (uint8_t i = 0; i < TIME_DIV_COUNT; ++i) {
        float err = TIME_DIV_STEPS[i] - td;
        if (err < 0.0f) err = -err;
        if (err < best_err) {
            best_err = err;
            best = i;
        }
    }
    return best;
}

inline uint8_t wrap_formula_delta(uint8_t base, int delta) {
    const int count = (int)BytebeatGraph::FORMULA_COUNT;
    int v = ((int)base + delta) % count;
    if (v < 0) v += count;
    return (uint8_t)v;
}

inline uint8_t toggle_mask_bit(uint8_t mask, uint8_t bit) {
    return (uint8_t)(mask ^ (uint8_t)(1u << (bit & 7u)));
}

inline uint8_t clamp_u8_delta(uint8_t base, int delta) {
    int v = (int)base + delta;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

static constexpr uint32_t kSnapshotFadeMinSamples = (AudioEngine::SAMPLE_RATE * 30u) / 1000u;

enum class FormulaFamily : uint8_t {
    T_MELODY = 0,
    BIT_HARMONY,
    PERCUSSIVE,
    CHAOS,
    HYBRID
};

inline FormulaFamily formula_family_of(uint8_t formula_id) {
    const uint8_t id = formula_id % BytebeatGraph::FORMULA_COUNT;
    if (id <= 3u) return FormulaFamily::T_MELODY;
    if (id <= 7u) return FormulaFamily::BIT_HARMONY;
    if (id <= 11u) return FormulaFamily::PERCUSSIVE;
    if (id <= 14u) return FormulaFamily::CHAOS;
    return FormulaFamily::HYBRID;
}

inline uint8_t family_count(FormulaFamily family) {
    switch (family) {
        case FormulaFamily::T_MELODY:   return 4u;
        case FormulaFamily::BIT_HARMONY:return 4u;
        case FormulaFamily::PERCUSSIVE: return 4u;
        case FormulaFamily::CHAOS:      return 3u;
        case FormulaFamily::HYBRID:     return 2u;
        default:                        return 1u;
    }
}

inline uint8_t family_base(FormulaFamily family) {
    switch (family) {
        case FormulaFamily::T_MELODY:   return 0u;
        case FormulaFamily::BIT_HARMONY:return 4u;
        case FormulaFamily::PERCUSSIVE: return 8u;
        case FormulaFamily::CHAOS:      return 12u;
        case FormulaFamily::HYBRID:     return 15u;
        default:                        return 0u;
    }
}

inline uint8_t pick_formula_from_family(FormulaFamily family, uint8_t choice) {
    const uint8_t count = family_count(family);
    return (uint8_t)(family_base(family) + (choice % count));
}

inline FormulaFamily pick_family_for_wild(FormulaFamily current, uint8_t choice) {
    switch (current) {
        case FormulaFamily::T_MELODY: {
            static constexpr FormulaFamily opts[3] = {FormulaFamily::T_MELODY, FormulaFamily::HYBRID, FormulaFamily::BIT_HARMONY};
            return opts[choice % 3u];
        }
        case FormulaFamily::BIT_HARMONY: {
            static constexpr FormulaFamily opts[3] = {FormulaFamily::BIT_HARMONY, FormulaFamily::HYBRID, FormulaFamily::CHAOS};
            return opts[choice % 3u];
        }
        case FormulaFamily::PERCUSSIVE: {
            static constexpr FormulaFamily opts[3] = {FormulaFamily::PERCUSSIVE, FormulaFamily::CHAOS, FormulaFamily::HYBRID};
            return opts[choice % 3u];
        }
        case FormulaFamily::CHAOS: {
            static constexpr FormulaFamily opts[2] = {FormulaFamily::CHAOS, FormulaFamily::HYBRID};
            return opts[choice % 2u];
        }
        case FormulaFamily::HYBRID: {
            static constexpr FormulaFamily opts[4] = {FormulaFamily::HYBRID, FormulaFamily::T_MELODY, FormulaFamily::BIT_HARMONY, FormulaFamily::CHAOS};
            return opts[choice % 4u];
        }
        default:
            return current;
    }
}

}



void StateManager::sanitize_snapshot(Snapshot& s, uint8_t slot_hint) {
    s.snapshot_version = SNAPSHOT_ENGINE_VERSION;
    if (s.seed == 0u) s.seed = generate_seed(slot_hint);
    if (s.zone > 4u) s.zone = 4u;
    s.macro = clamp01(s.macro);
    s.glide_time = clamp01(s.glide_time);
    s.time_div = quantize_time_div(clamp01((quantized_time_div_index(s.time_div)) / (float)(TIME_DIV_COUNT - 1)));
    s.tonal = clamp01(s.tonal);
    s.spread = clamp01(s.spread);
    s.filter_cutoff = clamp01(s.filter_cutoff);
    s.fx_amount = clamp01(s.fx_amount);
    s.drive = clamp01(s.drive);
    s.reverb_room = clamp01(s.reverb_room);
    s.reverb_wet = clamp01(s.reverb_wet);
    s.delay_div = clamp01(s.delay_div);
    s.delay_wet = clamp01(s.delay_wet);
    s.scale_id = (s.scale_id < (uint8_t)ScaleId::NUM_SCALES) ? s.scale_id : (uint8_t)ScaleId::MAJOR;
    s.root = (s.root < 12u) ? s.root : 0u;
    s.drum_color = clamp01(s.drum_color);
    s.drum_decay = clamp01(s.drum_decay);
    s.env_release = clamp01(s.env_release);
    s.env_attack = clamp01(s.env_attack);
    s.formula_a = s.formula_a % BytebeatGraph::FORMULA_COUNT;
    s.formula_b = s.formula_b % BytebeatGraph::FORMULA_COUNT;
    s.shift = (s.shift > 7u) ? 7u : s.shift;
}

void StateManager::apply_snapshot_engine_to_graph(BytebeatGraph& graph, const Snapshot& s) const {
    graph.set_formula_a(s.formula_a);
    graph.set_formula_b(s.formula_b);
    graph.set_morph(s.morph);
    graph.set_rate(s.rate);
    graph.set_shift(s.shift);
    graph.set_mask(s.mask);
    graph.set_feedback(s.feedback);
    graph.set_jitter(s.jitter);
    graph.set_phase(s.phase);
    graph.set_xor_fold(s.xor_fold);
    graph.set_seed_mod(s.bb_seed);
    graph.set_filter_macro(s.filter_macro);
    graph.set_resonance(s.resonance);
    graph.set_env_macro(s.env_macro);
}

void StateManager::sync_live_from_snapshot(const Snapshot& s, bool rebuild_graph) {
    begin_live_write();
    if (rebuild_graph) {
        ZoneConfig cfg = make_zone(s.zone);
        graphs_[active_graph_].generate(s.seed, s.zone, cfg);
        apply_snapshot_engine_to_graph(graphs_[active_graph_], s);
        glide_.stop();
        pending_slot_ = NO_PENDING;
        ++snapshot_epoch_;
    }

    ctx_.zone      = s.zone;
    ctx_.macro     = s.macro;
    ctx_.tonal     = s.tonal;
    ctx_.time_div  = s.time_div;
    ctx_.spread    = s.spread;

    scale_id_live_    = s.scale_id;
    root_live_        = s.root;
    drum_color_live_  = s.drum_color;
    drum_decay_live_  = s.drum_decay;
    drive_live_       = s.drive;
    glide_live_       = s.glide_time;
    hp_live_          = clamp01(s.filter_cutoff);
    chorus_live_      = clamp01(s.fx_amount);
    reverb_room_live_ = s.reverb_room;
    reverb_wet_live_  = s.reverb_wet;
    delay_div_live_   = s.delay_div;
    delay_wet_live_   = s.delay_wet;
    env_release_live_ = s.env_release;
    env_attack_live_  = s.env_attack;
    env_loop_live_    = s.env_loop;

    encoder_.scale_id = scale_id_live_;
    encoder_.root     = root_live_;
    seed_variation_live_ = 0.0f;
    end_live_write();
}


static uint8_t lerp_u8_local(uint8_t a, uint8_t b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    const float v = (1.0f - t) * float(a) + t * float(b);
    return (uint8_t)(v + 0.5f);
}

static float smoothstep_local(float t) {
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

static float morph_curve_local(float t) {
    const float s = smoothstep_local(t);
    return smoothstep_local(s);
}

static uint8_t clamp_u8_local(int v, int lo, int hi) {
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (uint8_t)v;
}

void StateManager::apply_snapshot_morph_preview(float amount) {
    if (!snapshot_morph_active_) return;
    begin_live_write();
    amount = clamp01(amount);
    snapshot_morph_amount_ = amount;

    const float t = morph_curve_local(amount);
    const float inv = 1.0f - t;

    const Snapshot& a = snapshots_[snapshot_morph_a_];
    const Snapshot& b = snapshots_[snapshot_morph_b_];
    BytebeatGraph& g = graphs_[active_graph_];

    // Fórmula representativa por snapshot: elegimos el lado dominante del morph interno.
    const uint8_t rep_a = (a.morph < 128u) ? a.formula_a : a.formula_b;
    const uint8_t rep_b = (b.morph < 128u) ? b.formula_a : b.formula_b;

    g.set_formula_a(rep_a);
    g.set_formula_b(rep_b);
    g.set_morph((uint8_t)(t * 255.0f + 0.5f));
    g.set_rate(lerp_u8_local(a.rate, b.rate, t));
    g.set_shift((uint8_t)(lerp_u8_local(a.shift, b.shift, t) & 7u));
    g.set_mask(clamp_u8_local((int)lerp_u8_local(a.mask, b.mask, t), 8, 255));
    g.set_feedback(clamp_u8_local((int)lerp_u8_local(a.feedback, b.feedback, t), 0, 200));
    g.set_jitter(clamp_u8_local((int)lerp_u8_local(a.jitter, b.jitter, t), 0, 180));
    g.set_phase(lerp_u8_local(a.phase, b.phase, t));
    g.set_xor_fold(lerp_u8_local(a.xor_fold, b.xor_fold, t));
    // Seed y selección de fórmulas se resuelven por lado dominante para evitar regiones erráticas.
    g.set_seed_mod((t < 0.5f) ? a.bb_seed : b.bb_seed);
    g.set_filter_macro(lerp_u8_local(a.filter_macro, b.filter_macro, t));
    g.set_resonance(lerp_u8_local(a.resonance, b.resonance, t));
    g.set_env_macro(lerp_u8_local(a.env_macro, b.env_macro, t));

    ctx_.macro        = inv * a.macro         + t * b.macro;
    ctx_.tonal        = inv * a.tonal         + t * b.tonal;
    ctx_.time_div     = inv * a.time_div      + t * b.time_div;
    ctx_.spread       = inv * a.spread        + t * b.spread;
    ctx_.zone         = (t < 0.5f) ? a.zone : b.zone;
    ctx_.quant_amount = Quantizer::pot_to_amount(ctx_.tonal);

    drive_live_       = inv * a.drive         + t * b.drive;
    glide_live_       = inv * a.glide_time    + t * b.glide_time;
    hp_live_          = inv * a.filter_cutoff + t * b.filter_cutoff;
    chorus_live_      = inv * a.fx_amount     + t * b.fx_amount;
    reverb_room_live_ = inv * a.reverb_room   + t * b.reverb_room;
    reverb_wet_live_  = inv * a.reverb_wet    + t * b.reverb_wet;
    delay_div_live_   = inv * a.delay_div     + t * b.delay_div;
    delay_wet_live_   = inv * a.delay_wet     + t * b.delay_wet;
    env_release_live_ = inv * a.env_release   + t * b.env_release;
    env_attack_live_  = inv * a.env_attack    + t * b.env_attack;
    env_loop_live_    = (t < 0.5f) ? a.env_loop : b.env_loop;
    drum_color_live_  = inv * a.drum_color    + t * b.drum_color;
    drum_decay_live_  = inv * a.drum_decay    + t * b.drum_decay;
    scale_id_live_    = (t < 0.5f) ? a.scale_id : b.scale_id;
    root_live_        = (t < 0.5f) ? a.root : b.root;
    encoder_.scale_id = scale_id_live_;
    encoder_.root     = root_live_;
    end_live_write();
}

bool StateManager::start_snapshot_morph(uint8_t slot_a, uint8_t slot_b) {
    if (slot_a >= NUM_SNAPSHOTS || slot_b >= NUM_SNAPSHOTS || slot_a == slot_b) return false;
    if (!snapshots_[slot_a].valid || !snapshots_[slot_b].valid) return false;

    snapshot_morph_active_ = true;
    snapshot_morph_a_ = slot_a;
    snapshot_morph_b_ = slot_b;
    snapshot_morph_amount_ = 0.0f;
    snapshot_morph_target_ = 0.0f;

    // Tomamos A como base inmediatamente para que el morph nazca desde un patch estable.
    active_slot_ = slot_a;
    sync_live_from_snapshot(snapshots_[slot_a], true);
    apply_snapshot_morph_preview(0.0f);
    return true;
}

void StateManager::update_snapshot_morph(float amount) {
    if (!snapshot_morph_active_) return;
    snapshot_morph_target_ = clamp01(amount);
    const float delta = snapshot_morph_target_ - snapshot_morph_amount_;
    const float stepped = snapshot_morph_amount_ + delta * 0.35f;
    apply_snapshot_morph_preview(stepped);
}

void StateManager::stop_snapshot_morph(bool commit_to_target) {
    if (!snapshot_morph_active_) return;

    const uint8_t slot_a = snapshot_morph_a_;
    const uint8_t slot_b = snapshot_morph_b_;
    snapshot_morph_active_ = false;
    snapshot_morph_a_ = 0xFFu;
    snapshot_morph_b_ = 0xFFu;
    snapshot_morph_amount_ = 0.0f;
    snapshot_morph_target_ = 0.0f;

    if (commit_to_target) {
        if (slot_b < NUM_SNAPSHOTS && snapshots_[slot_b].valid) {
            do_trigger(slot_b);
        }
    } else if (slot_a < NUM_SNAPSHOTS && snapshots_[slot_a].valid) {
        active_slot_ = slot_a;
        sync_live_from_snapshot(snapshots_[slot_a], true);
    }
}

void StateManager::load_factory_bank() {
    Snapshot base{};
    base.snapshot_version = SNAPSHOT_ENGINE_VERSION;
    base.seed            = generate_seed(0);
    base.zone            = 1;
    base.macro           = 0.50f;
    base.glide_time      = 0.04f;
    base.time_div        = 1.0f;
    base.tonal           = 0.50f;
    base.spread          = 0.20f;
    base.filter_cutoff   = 0.36f;
    base.fx_amount       = 0.12f;
    base.drive           = 0.16f;
    base.reverb_room     = 0.68f;
    base.reverb_wet      = 0.18f;
    base.delay_div       = 0.36f;
    base.delay_wet       = 0.06f;
    base.scale_id        = (uint8_t)ScaleId::DORIAN;
    base.root            = 0;
    base.drum_color      = 0.20f;
    base.drum_decay      = 0.52f;
    base.env_release     = 0.08f;
    base.env_attack      = 0.01f;
    base.env_loop        = false;
    base.formula_a       = 0;
    base.formula_b       = 8;
    base.morph           = 96;
    base.rate            = 112;
    base.shift           = 2;
    base.mask            = 192;
    base.feedback        = 18;
    base.jitter          = 8;
    base.phase           = 48;
    base.xor_fold        = 0;
    base.bb_seed         = 16;
    base.filter_macro    = 108;
    base.resonance       = 20;
    base.env_macro       = 64;
    base.valid           = true;

    for (uint8_t i = 0; i < NUM_SNAPSHOTS; ++i) {
        snapshots_[i] = base;
        snapshots_[i].seed = generate_seed(i) ^ ((uint32_t)(i + 1) * 0x11001u);
        sanitize_snapshot(snapshots_[i], i);
    }

    // 0 — Fractal Bloom: morph melódico/drone, entrada amigable y abierta.
    snapshots_[0].zone          = 1;
    snapshots_[0].macro         = 0.44f;
    snapshots_[0].glide_time    = 0.08f;
    snapshots_[0].time_div      = 1.0f;
    snapshots_[0].tonal         = 0.80f;
    snapshots_[0].spread        = 0.12f;
    snapshots_[0].filter_cutoff = 0.22f;
    snapshots_[0].fx_amount     = 0.08f;
    snapshots_[0].drive         = 0.10f;
    snapshots_[0].reverb_room   = 0.74f;
    snapshots_[0].reverb_wet    = 0.18f;
    snapshots_[0].delay_wet     = 0.04f;
    snapshots_[0].scale_id      = (uint8_t)ScaleId::DORIAN;
    snapshots_[0].formula_a     = 0;
    snapshots_[0].formula_b     = 8;
    snapshots_[0].morph         = 84;
    snapshots_[0].rate          = 112;
    snapshots_[0].shift         = 2;
    snapshots_[0].mask          = 188;
    snapshots_[0].feedback      = 14;
    snapshots_[0].jitter        = 6;
    snapshots_[0].phase         = 56;
    snapshots_[0].bb_seed       = 12;
    snapshots_[0].filter_macro  = 88;
    snapshots_[0].resonance     = 14;
    snapshots_[0].env_macro     = 28;

    // 1 — Iron Pulse: bass/groove más estable y con menos aspereza en feedback.
    snapshots_[1].zone          = 2;
    snapshots_[1].macro         = 0.56f;
    snapshots_[1].glide_time    = 0.03f;
    snapshots_[1].time_div      = 0.5f;
    snapshots_[1].tonal         = 0.54f;
    snapshots_[1].spread        = 0.20f;
    snapshots_[1].filter_cutoff = 0.44f;
    snapshots_[1].fx_amount     = 0.12f;
    snapshots_[1].drive         = 0.18f;
    snapshots_[1].reverb_room   = 0.58f;
    snapshots_[1].reverb_wet    = 0.12f;
    snapshots_[1].delay_div     = 0.42f;
    snapshots_[1].delay_wet     = 0.06f;
    snapshots_[1].scale_id      = (uint8_t)ScaleId::NAT_MINOR;
    snapshots_[1].root          = 7;
    snapshots_[1].drum_color    = 0.26f;
    snapshots_[1].drum_decay    = 0.58f;
    snapshots_[1].env_release   = 0.05f;
    snapshots_[1].formula_a     = 3;
    snapshots_[1].formula_b     = 4;
    snapshots_[1].morph         = 42;
    snapshots_[1].rate          = 144;
    snapshots_[1].shift         = 3;
    snapshots_[1].mask          = 206;
    snapshots_[1].feedback      = 34;
    snapshots_[1].jitter        = 6;
    snapshots_[1].phase         = 76;
    snapshots_[1].xor_fold      = 14;
    snapshots_[1].bb_seed       = 33;
    snapshots_[1].filter_macro  = 122;
    snapshots_[1].resonance     = 28;
    snapshots_[1].env_macro     = 88;

    // 2 — Glass Circuit: perc/glitch recortado, HP moderado y menos descontrol.
    snapshots_[2].zone          = 4;
    snapshots_[2].macro         = 0.64f;
    snapshots_[2].glide_time    = 0.01f;
    snapshots_[2].time_div      = 1.0f;
    snapshots_[2].tonal         = 0.18f;
    snapshots_[2].spread        = 0.36f;
    snapshots_[2].filter_cutoff = 0.70f;
    snapshots_[2].fx_amount     = 0.18f;
    snapshots_[2].drive         = 0.30f;
    snapshots_[2].reverb_room   = 0.52f;
    snapshots_[2].reverb_wet    = 0.08f;
    snapshots_[2].delay_div     = 0.28f;
    snapshots_[2].delay_wet     = 0.03f;
    snapshots_[2].scale_id      = (uint8_t)ScaleId::CHROMATIC;
    snapshots_[2].root          = 2;
    snapshots_[2].drum_color    = 0.38f;
    snapshots_[2].drum_decay    = 0.40f;
    snapshots_[2].env_release   = 0.03f;
    snapshots_[2].formula_a     = 5;
    snapshots_[2].formula_b     = 7;
    snapshots_[2].morph         = 116;
    snapshots_[2].rate          = 168;
    snapshots_[2].shift         = 4;
    snapshots_[2].mask          = 228;
    snapshots_[2].feedback      = 54;
    snapshots_[2].jitter        = 22;
    snapshots_[2].phase         = 148;
    snapshots_[2].xor_fold      = 120;
    snapshots_[2].bb_seed       = 74;
    snapshots_[2].filter_macro  = 166;
    snapshots_[2].resonance     = 34;
    snapshots_[2].env_macro     = 136;

    // 3 — Silt Air: ambient/drone con cola larga, más espacio que agresión.
    snapshots_[3].zone          = 3;
    snapshots_[3].macro         = 0.58f;
    snapshots_[3].glide_time    = 0.12f;
    snapshots_[3].time_div      = 2.0f;
    snapshots_[3].tonal         = 0.36f;
    snapshots_[3].spread        = 0.52f;
    snapshots_[3].filter_cutoff = 0.26f;
    snapshots_[3].fx_amount     = 0.24f;
    snapshots_[3].drive         = 0.16f;
    snapshots_[3].reverb_room   = 0.92f;
    snapshots_[3].reverb_wet    = 0.32f;
    snapshots_[3].delay_div     = 0.52f;
    snapshots_[3].delay_wet     = 0.18f;
    snapshots_[3].scale_id      = (uint8_t)ScaleId::PENTA_MIN;
    snapshots_[3].root          = 9;
    snapshots_[3].drum_color    = 0.18f;
    snapshots_[3].drum_decay    = 0.68f;
    snapshots_[3].env_release   = 0.26f;
    snapshots_[3].env_attack    = 0.10f;
    snapshots_[3].formula_a     = 8;
    snapshots_[3].formula_b     = 9;
    snapshots_[3].morph         = 156;
    snapshots_[3].rate          = 88;
    snapshots_[3].shift         = 1;
    snapshots_[3].mask          = 176;
    snapshots_[3].feedback      = 24;
    snapshots_[3].jitter        = 14;
    snapshots_[3].phase         = 112;
    snapshots_[3].bb_seed       = 57;
    snapshots_[3].filter_macro  = 74;
    snapshots_[3].resonance     = 44;
    snapshots_[3].env_macro     = 228;

    // 4 — Choir Bits: vocal/chip brillante pero usable musicalmente.
    snapshots_[4].zone          = 2;
    snapshots_[4].macro         = 0.48f;
    snapshots_[4].glide_time    = 0.02f;
    snapshots_[4].time_div      = 0.25f;
    snapshots_[4].tonal         = 0.76f;
    snapshots_[4].spread        = 0.14f;
    snapshots_[4].filter_cutoff = 0.48f;
    snapshots_[4].fx_amount     = 0.10f;
    snapshots_[4].drive         = 0.18f;
    snapshots_[4].reverb_room   = 0.56f;
    snapshots_[4].reverb_wet    = 0.16f;
    snapshots_[4].delay_div     = 0.34f;
    snapshots_[4].delay_wet     = 0.10f;
    snapshots_[4].scale_id      = (uint8_t)ScaleId::MAJOR;
    snapshots_[4].root          = 5;
    snapshots_[4].drum_color    = 0.28f;
    snapshots_[4].drum_decay    = 0.50f;
    snapshots_[4].env_release   = 0.04f;
    snapshots_[4].formula_a     = 10;
    snapshots_[4].formula_b     = 11;
    snapshots_[4].morph         = 122;
    snapshots_[4].rate          = 132;
    snapshots_[4].shift         = 2;
    snapshots_[4].mask          = 180;
    snapshots_[4].feedback      = 20;
    snapshots_[4].jitter        = 4;
    snapshots_[4].phase         = 68;
    snapshots_[4].xor_fold      = 10;
    snapshots_[4].bb_seed       = 92;
    snapshots_[4].filter_macro  = 132;
    snapshots_[4].resonance     = 24;
    snapshots_[4].env_macro     = 172;

    // 5 — Burned Tape FM: experimental roto, pero con menos clipping constante.
    snapshots_[5].zone          = 4;
    snapshots_[5].macro         = 0.68f;
    snapshots_[5].glide_time    = 0.01f;
    snapshots_[5].time_div      = 1.0f;
    snapshots_[5].tonal         = 0.16f;
    snapshots_[5].spread        = 0.60f;
    snapshots_[5].filter_cutoff = 0.78f;
    snapshots_[5].fx_amount     = 0.26f;
    snapshots_[5].drive         = 0.34f;
    snapshots_[5].reverb_room   = 0.42f;
    snapshots_[5].reverb_wet    = 0.08f;
    snapshots_[5].delay_div     = 0.20f;
    snapshots_[5].delay_wet     = 0.04f;
    snapshots_[5].scale_id      = (uint8_t)ScaleId::CHROMATIC;
    snapshots_[5].root          = 11;
    snapshots_[5].drum_color    = 0.48f;
    snapshots_[5].drum_decay    = 0.30f;
    snapshots_[5].env_release   = 0.02f;
    snapshots_[5].env_loop      = true;
    snapshots_[5].formula_a     = 12;
    snapshots_[5].formula_b     = 16;
    snapshots_[5].morph         = 138;
    snapshots_[5].rate          = 156;
    snapshots_[5].shift         = 3;
    snapshots_[5].mask          = 236;
    snapshots_[5].feedback      = 48;
    snapshots_[5].jitter        = 24;
    snapshots_[5].phase         = 132;
    snapshots_[5].xor_fold      = 184;
    snapshots_[5].bb_seed       = 132;
    snapshots_[5].filter_macro  = 170;
    snapshots_[5].resonance     = 40;
    snapshots_[5].env_macro     = 146;

    // 6 — Bright Ladder: arpegio brillante y directo para testear musicalidad.
    snapshots_[6].zone          = 1;
    snapshots_[6].macro         = 0.36f;
    snapshots_[6].glide_time    = 0.16f;
    snapshots_[6].time_div      = 4.0f;
    snapshots_[6].tonal         = 0.86f;
    snapshots_[6].spread        = 0.06f;
    snapshots_[6].filter_cutoff = 0.16f;
    snapshots_[6].fx_amount     = 0.06f;
    snapshots_[6].drive         = 0.08f;
    snapshots_[6].reverb_room   = 0.78f;
    snapshots_[6].reverb_wet    = 0.24f;
    snapshots_[6].delay_div     = 0.58f;
    snapshots_[6].delay_wet     = 0.20f;
    snapshots_[6].scale_id      = (uint8_t)ScaleId::MAJOR;
    snapshots_[6].root          = 0;
    snapshots_[6].drum_color    = 0.12f;
    snapshots_[6].drum_decay    = 0.60f;
    snapshots_[6].env_release   = 0.16f;
    snapshots_[6].env_attack    = 0.04f;
    snapshots_[6].formula_a     = 1;
    snapshots_[6].formula_b     = 15;
    snapshots_[6].morph         = 86;
    snapshots_[6].rate          = 100;
    snapshots_[6].shift         = 2;
    snapshots_[6].mask          = 164;
    snapshots_[6].feedback      = 12;
    snapshots_[6].jitter        = 2;
    snapshots_[6].phase         = 58;
    snapshots_[6].bb_seed       = 18;
    snapshots_[6].filter_macro  = 96;
    snapshots_[6].resonance     = 10;
    snapshots_[6].env_macro     = 116;

    // 7 — Controlled Ruin: caos final del banco, pero todavía performático.
    snapshots_[7].zone          = 4;
    snapshots_[7].macro         = 0.58f;
    snapshots_[7].glide_time    = 0.01f;
    snapshots_[7].time_div      = 0.5f;
    snapshots_[7].tonal         = 0.10f;
    snapshots_[7].spread        = 0.66f;
    snapshots_[7].filter_cutoff = 0.66f;
    snapshots_[7].fx_amount     = 0.16f;
    snapshots_[7].drive         = 0.28f;
    snapshots_[7].reverb_room   = 0.50f;
    snapshots_[7].reverb_wet    = 0.14f;
    snapshots_[7].delay_div     = 0.30f;
    snapshots_[7].delay_wet     = 0.06f;
    snapshots_[7].scale_id      = (uint8_t)ScaleId::CHROMATIC;
    snapshots_[7].root          = 3;
    snapshots_[7].drum_color    = 0.40f;
    snapshots_[7].drum_decay    = 0.36f;
    snapshots_[7].env_release   = 0.04f;
    snapshots_[7].formula_a     = 14;
    snapshots_[7].formula_b     = 5;
    snapshots_[7].morph         = 164;
    snapshots_[7].rate          = 180;
    snapshots_[7].shift         = 5;
    snapshots_[7].mask          = 226;
    snapshots_[7].feedback      = 62;
    snapshots_[7].jitter        = 30;
    snapshots_[7].phase         = 188;
    snapshots_[7].xor_fold      = 138;
    snapshots_[7].bb_seed       = 188;
    snapshots_[7].filter_macro  = 176;
    snapshots_[7].resonance     = 52;
    snapshots_[7].env_macro     = 142;
}

void StateManager::init() {
    uint lock_num = spin_lock_claim_unused(true);
    lock_         = spin_lock_instance(lock_num);

    load_factory_bank();

    if (FlashStore::load(snapshots_)) {
        LOG_AUDIO("STATE: snapshots cargados desde Flash OK");
    } else {
        LOG_AUDIO("STATE: Flash inválido — usando banco factory BYT3");
    }

    active_slot_   = 0;
    ctx_.t         = 0;
    ctx_.note_mode_active = false;
    ctx_.note_pitch_ratio = 1.0f;
    duck_amount_live_= 0.0f;
    sync_live_from_snapshot(snapshots_[0], true);
    ctx_.drum_color = drum_color_live_;
    ctx_.drum_decay = drum_decay_live_;
    encoder_.density_amount = 0.50f;
    encoder_.chaos_amount   = 0.25f;
    encoder_.space_amount   = 0.25f;

    env_release_live_   = snapshots_[0].env_release;
    env_attack_live_    = snapshots_[0].env_attack;
    env_loop_live_      = snapshots_[0].env_loop;
    env_loop_time_live_ = 1.0f;

    LOG_AUDIO("STATE: init OK — slot0 seed=0x%08lX zone=%u",
              (unsigned long)snapshots_[0].seed, snapshots_[0].zone);
}

uint32_t StateManager::generate_seed(uint8_t slot) {
    return 0xDEADBEEFu ^ (0x9E3779B9u * (uint32_t)(slot + 1));
}

uint32_t StateManager::rng_next() {
    random_state_ ^= (random_state_ << 13);
    random_state_ ^= (random_state_ >> 17);
    random_state_ ^= (random_state_ << 5);
    return random_state_;
}

float StateManager::rand01() {
    return (float)(rng_next() & 0x00FFFFFFu) / 16777215.0f;
}

float StateManager::rand_range(float lo, float hi) {
    return lo + (hi - lo) * rand01();
}

uint8_t StateManager::rand_u8(uint8_t max_exclusive) {
    return max_exclusive ? (uint8_t)(rng_next() % max_exclusive) : 0;
}


void StateManager::process_pending() {
    if (!glide_.is_active() && pending_slot_ != NO_PENDING) {
        active_graph_ = incoming_graph_;
        active_slot_  = pending_slot_;
        pending_slot_ = NO_PENDING;
        LOG_AUDIO("STATE: commit slot %u", active_slot_);
    }

    SnapshotEvent ev;
    bool has_ev = false;
    {
        uint32_t s = spin_lock_blocking(lock_);
        if (event_ready_) {
            ev          = pending_event_;
            event_ready_= false;
            has_ev      = true;
        }
        spin_unlock(lock_, s);
    }
    if (!has_ev) return;

    switch (ev.type) {
    case SnapshotEventType::TRIGGER: do_trigger(ev.slot); break;
    // SAVE legacy path eliminado V1.21: request_save() va directo a do_save() con spin_lock.
    default: break;
    }
}

void StateManager::do_trigger(uint8_t slot) {
    if (slot >= NUM_SNAPSHOTS || !snapshots_[slot].valid) return;
    snapshot_morph_active_ = false;
    snapshot_morph_a_ = 0xFFu;
    snapshot_morph_b_ = 0xFFu;
    snapshot_morph_amount_ = 0.0f;
    snapshot_morph_target_ = 0.0f;
    if (slot == active_slot_ && !glide_.is_active())      return;

    const Snapshot& s = snapshots_[slot];
    incoming_graph_   = 1 - active_graph_;

    ZoneConfig cfg = make_zone(s.zone);
    graphs_[incoming_graph_].generate(s.seed, s.zone, cfg);
    apply_snapshot_engine_to_graph(graphs_[incoming_graph_], s);

    uint32_t dur = (uint32_t)(s.glide_time * AudioEngine::SAMPLE_RATE);
    // v13: asegurar un crossfade mínimo performático incluso si glide_time es muy corto.
    if (dur < kSnapshotFadeMinSamples) dur = kSnapshotFadeMinSamples;
    glide_.start(&graphs_[active_graph_], &graphs_[incoming_graph_], dur);
    pending_slot_ = slot;

    sync_live_from_snapshot(s, false);

    LOG_AUDIO("STATE: trigger slot=%u zona=%u glide=%.2fs", slot, s.zone, s.glide_time);
}

void StateManager::do_save(uint8_t slot, const float pots[7]) {
    if (slot >= NUM_SNAPSHOTS) return;
    (void)pots; // compatibilidad legacy: el save toma el estado live real.
    Snapshot& s  = snapshots_[slot];
    s.snapshot_version = SNAPSHOT_ENGINE_VERSION;
    s.macro      = ctx_.macro;
    s.tonal      = ctx_.tonal;
    s.spread     = ctx_.spread;
    s.drive      = drive_live_;
    s.time_div   = ctx_.time_div;
    s.glide_time = glide_live_;
    s.reverb_room = reverb_room_live_;
    s.reverb_wet  = reverb_wet_live_;
    s.delay_div   = delay_div_live_;    // V1.18
    s.delay_wet   = delay_wet_live_;
    s.scale_id    = scale_id_live_;
    s.root        = root_live_;
    s.drum_color  = drum_color_live_;   // guarda el valor performático actual
    s.drum_decay  = drum_decay_live_;
    s.valid       = true;
    s.zone        = graphs_[active_graph_].get_zone();
    s.seed        = graphs_[active_graph_].get_seed();
    s.formula_a   = graphs_[active_graph_].get_formula_a();
    s.formula_b   = graphs_[active_graph_].get_formula_b();
    s.morph       = graphs_[active_graph_].get_morph();
    s.rate        = graphs_[active_graph_].get_rate();
    s.shift       = graphs_[active_graph_].get_shift();
    s.mask        = graphs_[active_graph_].get_mask();
    s.feedback    = graphs_[active_graph_].get_feedback();
    s.jitter      = graphs_[active_graph_].get_jitter();
    s.phase       = graphs_[active_graph_].get_phase();
    s.xor_fold    = graphs_[active_graph_].get_xor_fold();
    s.bb_seed     = graphs_[active_graph_].get_seed_mod();
    s.filter_macro = graphs_[active_graph_].get_filter_macro();
    s.resonance   = graphs_[active_graph_].get_resonance();
    s.env_macro   = graphs_[active_graph_].get_env_macro();
    s.env_release = env_release_live_;
    s.env_attack  = env_attack_live_;
    s.env_loop    = env_loop_live_;
    // FIX V1.21 B4: HP y chorus live no se guardaban en do_save (sí en mutate/randomize).
    // Sin esto, un SAVE manual perdía los valores de HP Filter y Chorus del snapshot.
    s.filter_cutoff = hp_live_;
    s.fx_amount     = chorus_live_;
    LOG_AUDIO("STATE: save slot=%u dc=%.2f dd=%.2f hp=%.2f fx=%.2f",
              slot, s.drum_color, s.drum_decay, s.filter_cutoff, s.fx_amount);
}

void StateManager::fill_context(EvalContext& ctx) const {
    ctx.macro         = ctx_.macro;
    ctx.tonal         = ctx_.tonal;
    ctx.zone          = ctx_.zone;
    ctx.time_div      = ctx_.time_div;
    ctx.spread        = ctx_.spread;
    ctx.quant_amount  = ctx_.quant_amount;
    ctx.scale_id      = scale_id_live_;
    ctx.root          = root_live_;
    // V1.7: drum params en contexto
    ctx.drum_color    = drum_color_live_;
    ctx.drum_decay    = drum_decay_live_;
    // V1.7: Note Mode
    ctx.note_mode_active = note_mode_active_;
    ctx.note_pitch_ratio = note_pitch_ratio_;
    // Stage 10A: high-level macro semantics are derived in AudioEngine,
    // but ctx exposes defaults so downstream code always sees valid values.
    ctx.breath_amount = 0.0f;
    ctx.harmonic_blend = 0.5f;
}

int16_t StateManager::evaluate(const EvalContext& ctx) {
    if (glide_.is_active()) return glide_.evaluate(ctx);
    return graphs_[active_graph_].evaluate(ctx);
}

int16_t StateManager::evaluate_preview(const EvalContext& ctx) const {
    if (glide_.is_active()) return glide_.preview_evaluate(ctx);
    return graphs_[active_graph_].preview_evaluate(ctx);
}

void StateManager::request_trigger(uint8_t slot) {
    uint32_t s = spin_lock_blocking(lock_);
    pending_event_.type = SnapshotEventType::TRIGGER;
    pending_event_.slot = slot;
    event_ready_        = true;
    spin_unlock(lock_, s);
}

void StateManager::request_save(uint8_t slot, const float pots[7]) {
    if (slot >= NUM_SNAPSHOTS) return;
    uint32_t s = spin_lock_blocking(lock_);
    do_save(slot, pots);
    spin_unlock(lock_, s);
}


void StateManager::set_graph_param_on_live_graphs(ParamId id, uint8_t value) {
    switch (id) {
    case PARAM_FORMULA_A:
        graphs_[active_graph_].set_formula_a(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_formula_a(value);
        break;
    case PARAM_FORMULA_B:
        graphs_[active_graph_].set_formula_b(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_formula_b(value);
        break;
    case PARAM_MORPH:
        graphs_[active_graph_].set_morph(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_morph(value);
        break;
    case PARAM_RATE:
        graphs_[active_graph_].set_rate(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_rate(value);
        break;
    case PARAM_SHIFT:
        graphs_[active_graph_].set_shift(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_shift(value);
        break;
    case PARAM_MASK:
        graphs_[active_graph_].set_mask(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_mask(value);
        break;
    case PARAM_FEEDBACK:
        graphs_[active_graph_].set_feedback(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_feedback(value);
        break;
    case PARAM_JITTER:
        graphs_[active_graph_].set_jitter(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_jitter(value);
        break;
    case PARAM_PHASE:
        graphs_[active_graph_].set_phase(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_phase(value);
        break;
    case PARAM_XOR_FOLD:
        graphs_[active_graph_].set_xor_fold(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_xor_fold(value);
        break;
    case PARAM_BB_SEED:
        graphs_[active_graph_].set_seed_mod(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_seed_mod(value);
        break;
    case PARAM_FILTER_MACRO:
        graphs_[active_graph_].set_filter_macro(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_filter_macro(value);
        break;
    case PARAM_RESONANCE:
        graphs_[active_graph_].set_resonance(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_resonance(value);
        break;
    case PARAM_ENV_MACRO:
        graphs_[active_graph_].set_env_macro(value);
        if (pending_slot_ != NO_PENDING) graphs_[incoming_graph_].set_env_macro(value);
        break;
    default:
        break;
    }
    end_live_write();
}

bool StateManager::apply_graph_param(ParamId id, float value) {
    const float v = clamp01(value);
    uint8_t mapped = 0;
    switch (id) {
    case PARAM_FORMULA_A:
    case PARAM_FORMULA_B:
        mapped = static_cast<uint8_t>(v * (BytebeatGraph::FORMULA_COUNT - 1) + 0.5f);
        break;
    case PARAM_SHIFT:
        mapped = static_cast<uint8_t>(v * 7.0f + 0.5f);
        break;
    case PARAM_MORPH:
    case PARAM_RATE:
    case PARAM_MASK:
    case PARAM_FEEDBACK:
    case PARAM_JITTER:
    case PARAM_PHASE:
    case PARAM_XOR_FOLD:
    case PARAM_BB_SEED:
    case PARAM_FILTER_MACRO:
    case PARAM_RESONANCE:
    case PARAM_ENV_MACRO:
        mapped = static_cast<uint8_t>(v * 255.0f + 0.5f);
        break;
    default:
        return false;
    }
    set_graph_param_on_live_graphs(id, mapped);
    return true;
}

void StateManager::set_patch_param(ParamId id, float value) {
    const float v = clamp01(value);
    if (apply_graph_param(id, v)) return;
    begin_live_write();
    switch (id) {
    case PARAM_MACRO:
        ctx_.macro = v;
        break;
    case PARAM_TONAL:
        ctx_.tonal = v;
        ctx_.quant_amount = Quantizer::pot_to_amount(v);
        break;
    case PARAM_SPREAD:
        ctx_.spread = v;
        break;
    case PARAM_DRIVE:
        drive_live_ = v;
        break;
    case PARAM_TIME_DIV:
        ctx_.time_div = quantize_time_div(v);
        break;
    case PARAM_SNAP_GATE:
        snap_live_ = v;
        break;
    case PARAM_GLIDE:
        glide_live_ = v * 2.0f;
        break;
    case PARAM_ENV_ATTACK:
        env_attack_live_ = v;
        break;
    case PARAM_ENV_RELEASE:
        env_release_live_ = v;
        break;
    case PARAM_BEAT_REPEAT_DIV:
        beat_repeat_div_live_ = v;
        break;
    case PARAM_GRAIN:
        grain_live_ = v;
        break;
    case PARAM_HP:
        hp_live_ = v;
        break;
    // V1.17: Delay
    case PARAM_DELAY_DIV:
        delay_div_live_ = v;
        break;
    case PARAM_DELAY_FB:
        delay_fb_live_ = v;
        break;
    case PARAM_DELAY_WET:
        delay_wet_live_ = v;
        break;
    default:
        break;
    }
    end_live_write();
}

void StateManager::set_bus_param(ParamId id, float value) {
    const float v = clamp01(value);
    if (apply_graph_param(id, v)) return;
    begin_live_write();
    switch (id) {
    case PARAM_REVERB_ROOM:
        reverb_room_live_ = v;
        break;
    case PARAM_REVERB_WET:
        reverb_wet_live_ = v;
        break;
    case PARAM_CHORUS:
        chorus_live_ = v;
        break;
    case PARAM_DRUM_DECAY:
        drum_decay_live_ = v;
        break;
    case PARAM_DRUM_COLOR:
        drum_color_live_ = v;
        break;
    case PARAM_DUCK_AMOUNT:
        duck_amount_live_ = v;
        break;
    // V1.17: Delay — también accesibles como bus params (SHIFT+REC)
    case PARAM_DELAY_DIV:
        delay_div_live_ = v;
        break;
    case PARAM_DELAY_FB:
        delay_fb_live_ = v;
        break;
    case PARAM_DELAY_WET:
        delay_wet_live_ = v;
        break;
    default:
        break;
    }
    end_live_write();
}



void StateManager::apply_space_macro() {
    const float s = clamp01(encoder_.space_amount);
    const Snapshot& snap = snapshots_[active_slot_];
    const float base_room = snap.valid ? snap.reverb_room : 0.84f;
    const float base_wet = snap.valid ? snap.reverb_wet : 0.25f;
    const float base_delay_wet = snap.valid ? snap.delay_wet : 0.0f;
    reverb_room_live_ = clamp01(0.28f + base_room * 0.42f + s * 0.46f);
    reverb_wet_live_  = clamp01(base_wet * 0.35f + s * 0.70f);
    delay_wet_live_   = clamp01(base_delay_wet * 0.25f + s * 0.55f);
    delay_fb_live_    = clamp01(0.20f + s * 0.58f);
    chorus_live_      = clamp01(s * 0.42f);
    hp_live_          = clamp01((1.0f - s) * 0.22f);
}

void StateManager::set_space_macro(float v) {
    begin_live_write();
    encoder_.space_amount = clamp01(v);
    apply_space_macro();
    end_live_write();
}

void StateManager::prepare_note_voice_graph(BytebeatGraph& dst, uint8_t formula_source) const {
    if (formula_source == 0xFFu) formula_source = note_voice_source_;
    const BytebeatGraph& src = graphs_[active_graph_];
    const ZoneConfig cfg{};
    dst.generate(src.get_seed(), src.get_zone(), cfg);

    const uint8_t formula_a = src.get_formula_a();
    const uint8_t formula_b = src.get_formula_b();

    if (formula_source == 0u) {
        dst.set_formula_a(formula_a);
        dst.set_formula_b(formula_a);
        dst.set_morph(0u);
    } else if (formula_source == 1u) {
        dst.set_formula_a(formula_b);
        dst.set_formula_b(formula_b);
        dst.set_morph(0u);
    } else {
        dst.set_formula_a(formula_a);
        dst.set_formula_b(formula_b);
        dst.set_morph(src.get_morph());
    }

    dst.set_rate(src.get_rate());
    dst.set_shift(src.get_shift());
    dst.set_mask(src.get_mask());
    dst.set_feedback(src.get_feedback());
    dst.set_jitter(src.get_jitter());
    dst.set_phase(src.get_phase());
    dst.set_xor_fold(src.get_xor_fold());
    dst.set_seed_mod(src.get_seed_mod());
    dst.set_filter_macro(src.get_filter_macro());
    dst.set_resonance(src.get_resonance());
    dst.set_env_macro(src.get_env_macro());
}


void StateManager::prepare_snapshot_voice_graph(BytebeatGraph& dst, uint8_t slot) const {
    if (slot >= NUM_SNAPSHOTS) slot = active_slot_;
    const Snapshot& s = snapshots_[slot].valid ? snapshots_[slot] : snapshots_[active_slot_];
    const ZoneConfig cfg = make_zone(s.zone);
    dst.generate(s.seed, s.zone, cfg);
    apply_snapshot_engine_to_graph(dst, s);
}

bool StateManager::flash_save() { for (uint8_t i = 0; i < NUM_SNAPSHOTS; ++i) sanitize_snapshot(snapshots_[i], i); return FlashStore::save(snapshots_); }
bool StateManager::flash_load() {
    load_factory_bank();
    if (!FlashStore::load(snapshots_)) return false;
    for (uint8_t i = 0; i < NUM_SNAPSHOTS; ++i) sanitize_snapshot(snapshots_[i], i);
    if (active_slot_ >= NUM_SNAPSHOTS || !snapshots_[active_slot_].valid) active_slot_ = 0;
    sync_live_from_snapshot(snapshots_[active_slot_], true);
    ctx_.drum_color = drum_color_live_;
    ctx_.drum_decay = drum_decay_live_;
    // Resetear estado global performático a defaults conocidos, pero sin
    // reescribir inmediatamente el carácter espacial del snapshot recién cargado.
    encoder_.density_amount = 0.50f;
    encoder_.chaos_amount   = 0.25f;
    encoder_.space_amount   = 0.25f;
    return true;
}

float StateManager::get_param_normalized(ParamId id) const {
    switch (id) {
    case PARAM_MACRO:           return clamp01(ctx_.macro);
    case PARAM_TONAL:           return clamp01(ctx_.tonal);
    case PARAM_SPREAD:          return clamp01(ctx_.spread);
    case PARAM_DRIVE:           return clamp01(drive_live_);
    case PARAM_TIME_DIV:        return (float)quantized_time_div_index(ctx_.time_div) / (float)(TIME_DIV_COUNT - 1);
    case PARAM_SNAP_GATE:       return clamp01(snap_live_);
    case PARAM_GLIDE:           return clamp01(glide_live_ * 0.5f);
    case PARAM_ENV_ATTACK:      return clamp01(env_attack_live_);
    case PARAM_ENV_RELEASE:     return clamp01(env_release_live_);
    case PARAM_BEAT_REPEAT_DIV: return clamp01(beat_repeat_div_live_);
    case PARAM_GRAIN:           return clamp01(grain_live_);
    case PARAM_HP:              return clamp01(hp_live_);
    case PARAM_REVERB_ROOM:     return clamp01(reverb_room_live_);
    case PARAM_REVERB_WET:      return clamp01(reverb_wet_live_);
    case PARAM_CHORUS:          return clamp01(chorus_live_);
    case PARAM_DRUM_DECAY:      return clamp01(drum_decay_live_);
    case PARAM_DRUM_COLOR:      return clamp01(drum_color_live_);
    case PARAM_DUCK_AMOUNT:     return clamp01(duck_amount_live_);
    case PARAM_DELAY_DIV:       return clamp01(delay_div_live_);
    case PARAM_DELAY_FB:        return clamp01(delay_fb_live_);
    case PARAM_DELAY_WET:       return clamp01(delay_wet_live_);
    case PARAM_FORMULA_A:
        return (float)graphs_[active_graph_].get_formula_a() / (float)(BytebeatGraph::FORMULA_COUNT - 1);
    case PARAM_FORMULA_B:
        return (float)graphs_[active_graph_].get_formula_b() / (float)(BytebeatGraph::FORMULA_COUNT - 1);
    case PARAM_MORPH:           return graphs_[active_graph_].get_morph() / 255.0f;
    case PARAM_RATE:            return graphs_[active_graph_].get_rate() / 255.0f;
    case PARAM_SHIFT:           return graphs_[active_graph_].get_shift() / 7.0f;
    case PARAM_MASK:            return graphs_[active_graph_].get_mask() / 255.0f;
    case PARAM_FEEDBACK:        return graphs_[active_graph_].get_feedback() / 255.0f;
    case PARAM_JITTER:          return graphs_[active_graph_].get_jitter() / 255.0f;
    case PARAM_PHASE:           return graphs_[active_graph_].get_phase() / 255.0f;
    case PARAM_XOR_FOLD:        return graphs_[active_graph_].get_xor_fold() / 255.0f;
    case PARAM_BB_SEED:         return graphs_[active_graph_].get_seed_mod() / 255.0f;
    case PARAM_FILTER_MACRO:    return graphs_[active_graph_].get_filter_macro() / 255.0f;
    case PARAM_RESONANCE:       return graphs_[active_graph_].get_resonance() / 255.0f;
    case PARAM_ENV_MACRO:       return graphs_[active_graph_].get_env_macro() / 255.0f;
    default:                    return 0.0f;
    }
}

void StateManager::set_aftertouch_macro(float pressure) {
    float base_macro = snapshots_[active_slot_].macro;
    if (pressure < 0.01f) {
        ctx_.macro = base_macro;
    } else {
        ctx_.macro = base_macro + pressure * (1.0f - base_macro);
    }
}
void StateManager::mutate_active_snapshot(float amount, bool wild) {
    amount = clamp01(amount);
    Snapshot& s = snapshots_[active_slot_];

    if (!s.valid) {
        s.snapshot_version = SNAPSHOT_ENGINE_VERSION;
        s.seed          = generate_seed(active_slot_);
        s.zone          = ctx_.zone;
        s.macro         = ctx_.macro;
        s.glide_time    = glide_live_;
        s.time_div      = ctx_.time_div;
        s.tonal         = ctx_.tonal;
        s.spread        = ctx_.spread;
        s.filter_cutoff = hp_live_;
        s.fx_amount     = chorus_live_;
        s.drive         = drive_live_;
        s.reverb_room   = reverb_room_live_;
        s.reverb_wet    = reverb_wet_live_;
        s.delay_div     = delay_div_live_;  // V1.18
        s.delay_wet     = delay_wet_live_;
        s.scale_id      = scale_id_live_;
        s.root          = root_live_;
        s.drum_color    = drum_color_live_;
        s.drum_decay    = drum_decay_live_;
        s.env_release   = env_release_live_;
        s.env_attack    = env_attack_live_;
        s.env_loop      = env_loop_live_;
        s.formula_a     = graphs_[active_graph_].get_formula_a();
        s.formula_b     = graphs_[active_graph_].get_formula_b();
        s.morph         = graphs_[active_graph_].get_morph();
        s.rate          = graphs_[active_graph_].get_rate();
        s.shift         = graphs_[active_graph_].get_shift();
        s.mask          = graphs_[active_graph_].get_mask();
        s.feedback      = graphs_[active_graph_].get_feedback();
        s.jitter        = graphs_[active_graph_].get_jitter();
        s.phase         = graphs_[active_graph_].get_phase();
        s.xor_fold      = graphs_[active_graph_].get_xor_fold();
        s.bb_seed       = graphs_[active_graph_].get_seed_mod();
        s.filter_macro  = graphs_[active_graph_].get_filter_macro();
        s.resonance     = graphs_[active_graph_].get_resonance();
        s.env_macro     = graphs_[active_graph_].get_env_macro();
        s.valid         = true;
    }

    random_state_ ^= (uint32_t)to_ms_since_boot(get_absolute_time()) * 0x45D9F3Bu;
    random_state_ ^= ((uint32_t)active_slot_ + 1u) * 0x9E3779B9u;

    const float contour = wild ? (0.26f + amount * 0.24f)
                               : (0.08f + amount * 0.10f);

    // Mantener identidad general del patch: Formula A, time_div y filter_cutoff no se destruyen.
    s.macro       = biased_jitter_value(s.macro, contour, wild ? 0.18f : 0.08f, rand01());
    s.tonal       = jitter_in_range(s.tonal, contour, wild ? 0.18f : 0.08f, 0.18f, 0.95f, rand01());
    s.spread      = jitter_in_range(s.spread, contour, wild ? 0.16f : 0.07f, 0.00f, 0.70f, rand01());
    s.drive       = jitter_in_range(s.drive, contour, wild ? 0.18f : 0.08f, 0.00f, 0.72f, rand01());
    s.glide_time  = jitter_in_range(s.glide_time, contour, wild ? 0.16f : 0.06f, 0.02f, 1.10f, rand01());
    s.env_attack  = jitter_in_range(s.env_attack, contour, wild ? 0.12f : 0.05f, 0.0f, 0.35f, rand01());
    s.env_release = jitter_in_range(s.env_release, contour, wild ? 0.16f : 0.06f, 0.01f, 0.55f, rand01());
    s.fx_amount   = jitter_in_range(s.fx_amount, contour, wild ? 0.16f : 0.06f, 0.0f, 0.60f, rand01());

    if (wild) {
        s.reverb_room = jitter_in_range(s.reverb_room, amount, 0.12f, 0.45f, 0.95f, rand01());
        s.reverb_wet  = jitter_in_range(s.reverb_wet, amount, 0.10f, 0.05f, 0.45f, rand01());
        s.drum_color  = jitter_in_range(s.drum_color, amount, 0.16f, 0.0f, 0.85f, rand01());
        s.drum_decay  = jitter_in_range(s.drum_decay, amount, 0.15f, 0.18f, 0.88f, rand01());
        if (rand01() < (0.06f + amount * 0.12f)) {
            s.env_loop = !s.env_loop;
        }
    }

    // Mutate musical/intencional: la familia de Formula B guía el rango de exploración.
    const FormulaFamily family_b = formula_family_of(s.formula_b);
    if (!wild) {
        if (rand01() < (0.65f + amount * 0.20f)) {
            switch (family_b) {
                case FormulaFamily::T_MELODY:
                case FormulaFamily::BIT_HARMONY:
                case FormulaFamily::PERCUSSIVE:
                    s.formula_b = pick_formula_from_family(family_b, rand_u8(family_count(family_b)));
                    break;
                case FormulaFamily::CHAOS:
                    s.formula_b = pick_formula_from_family(rand01() < 0.65f ? FormulaFamily::CHAOS : FormulaFamily::HYBRID, rand_u8(4u));
                    break;
                case FormulaFamily::HYBRID:
                    s.formula_b = pick_formula_from_family(rand01() < 0.70f ? FormulaFamily::HYBRID : FormulaFamily::T_MELODY, rand_u8(4u));
                    break;
            }
        }

        s.morph    = clamp_u8_delta(s.morph,    (int)(rand01() * (20.0f + amount * 20.0f)) - (int)(10.0f + amount * 10.0f));
        s.phase    = clamp_u8_delta(s.phase,    (int)(rand01() * (24.0f + amount * 20.0f)) - (int)(12.0f + amount * 10.0f));
        s.bb_seed  = clamp_u8_delta(s.bb_seed,  (int)(rand01() * (20.0f + amount * 24.0f)) - (int)(10.0f + amount * 12.0f));

        switch (family_b) {
            case FormulaFamily::T_MELODY:
                s.mask     = clamp_u8_delta(s.mask,     (int)(rand01() * (12.0f + amount * 12.0f)) - (int)(6.0f + amount * 6.0f));
                s.shift    = clamp_u8_delta(s.shift,    ((rand01() < 0.5f) ? -1 : 1));
                s.rate     = clamp_u8_delta(s.rate,     (int)(rand01() * (14.0f + amount * 12.0f)) - (int)(7.0f + amount * 6.0f));
                s.feedback = clamp_u8_delta(s.feedback, (int)(rand01() * 12.0f) - 6);
                s.jitter   = clamp_u8_delta(s.jitter,   (int)(rand01() * 10.0f) - 5);
                break;
            case FormulaFamily::BIT_HARMONY:
                s.mask     = clamp_u8_delta(s.mask,     (int)(rand01() * (18.0f + amount * 18.0f)) - (int)(9.0f + amount * 9.0f));
                s.shift    = clamp_u8_delta(s.shift,    (int)(rand01() * 3.0f) - 1);
                s.feedback = clamp_u8_delta(s.feedback, (int)(rand01() * (18.0f + amount * 12.0f)) - (int)(9.0f + amount * 6.0f));
                s.resonance= clamp_u8_delta(s.resonance,(int)(rand01() * 20.0f) - 10);
                s.xor_fold = clamp_u8_delta(s.xor_fold, (int)(rand01() * 14.0f) - 7);
                break;
            case FormulaFamily::PERCUSSIVE:
                s.mask     = clamp_u8_delta(s.mask,     (int)(rand01() * (16.0f + amount * 14.0f)) - (int)(8.0f + amount * 7.0f));
                s.rate     = clamp_u8_delta(s.rate,     (int)(rand01() * (22.0f + amount * 18.0f)) - (int)(11.0f + amount * 9.0f));
                s.feedback = clamp_u8_delta(s.feedback, (int)(rand01() * 12.0f) - 6);
                s.jitter   = clamp_u8_delta(s.jitter,   (int)(rand01() * 12.0f) - 6);
                s.env_macro= clamp_u8_delta(s.env_macro,(int)(rand01() * 20.0f) - 10);
                break;
            case FormulaFamily::CHAOS:
                s.mask     = clamp_u8_delta(s.mask,     (int)(rand01() * (22.0f + amount * 18.0f)) - (int)(11.0f + amount * 9.0f));
                s.shift    = clamp_u8_delta(s.shift,    (int)(rand01() * 5.0f) - 2);
                s.bb_seed  = clamp_u8_delta(s.bb_seed,  (int)(rand01() * (36.0f + amount * 24.0f)) - (int)(18.0f + amount * 12.0f));
                s.xor_fold = clamp_u8_delta(s.xor_fold, (int)(rand01() * (24.0f + amount * 20.0f)) - (int)(12.0f + amount * 10.0f));
                s.feedback = clamp_u8_delta(s.feedback, (int)(rand01() * 18.0f) - 9);
                break;
            case FormulaFamily::HYBRID:
                s.mask     = clamp_u8_delta(s.mask,     (int)(rand01() * (18.0f + amount * 16.0f)) - (int)(9.0f + amount * 8.0f));
                s.shift    = clamp_u8_delta(s.shift,    (int)(rand01() * 3.0f) - 1);
                s.rate     = clamp_u8_delta(s.rate,     (int)(rand01() * (16.0f + amount * 14.0f)) - (int)(8.0f + amount * 7.0f));
                s.feedback = clamp_u8_delta(s.feedback, (int)(rand01() * 14.0f) - 7);
                s.jitter   = clamp_u8_delta(s.jitter,   (int)(rand01() * 12.0f) - 6);
                s.xor_fold = clamp_u8_delta(s.xor_fold, (int)(rand01() * 16.0f) - 8);
                break;
        }

        if (rand01() < (0.30f + amount * 0.20f)) {
            s.resonance = clamp_u8_delta(s.resonance, (int)(rand01() * 24.0f) - 12);
        }
        if (rand01() < (0.24f + amount * 0.16f)) {
            s.env_macro = clamp_u8_delta(s.env_macro, (int)(rand01() * 28.0f) - 14);
        }
    } else {
        if (rand01() < (0.85f + amount * 0.10f)) {
            const FormulaFamily target = pick_family_for_wild(family_b, rand_u8(8u));
            s.formula_b = pick_formula_from_family(target, rand_u8(family_count(target)));
        }

        s.morph       = clamp_u8_delta(s.morph,       (int)(rand01() * (64.0f + amount * 40.0f)) - (int)(32.0f + amount * 20.0f));
        s.phase       = clamp_u8_delta(s.phase,       (int)(rand01() * (84.0f + amount * 48.0f)) - (int)(42.0f + amount * 24.0f));
        s.bb_seed     = clamp_u8_delta(s.bb_seed,     (int)(rand01() * (80.0f + amount * 80.0f)) - (int)(40.0f + amount * 40.0f));
        s.feedback    = clamp_u8_delta(s.feedback,    (int)(rand01() * (48.0f + amount * 36.0f)) - (int)(24.0f + amount * 18.0f));
        s.jitter      = clamp_u8_delta(s.jitter,      (int)(rand01() * (44.0f + amount * 40.0f)) - (int)(22.0f + amount * 20.0f));
        s.xor_fold    = clamp_u8_delta(s.xor_fold,    (int)(rand01() * (72.0f + amount * 56.0f)) - (int)(36.0f + amount * 28.0f));
        s.rate        = clamp_u8_delta(s.rate,        (int)(rand01() * (36.0f + amount * 36.0f)) - (int)(18.0f + amount * 18.0f));
        s.mask        = clamp_u8_delta(s.mask,        (int)(rand01() * (72.0f + amount * 56.0f)) - (int)(36.0f + amount * 28.0f));
        s.resonance   = clamp_u8_delta(s.resonance,   (int)(rand01() * 48.0f) - 24);
        s.env_macro   = clamp_u8_delta(s.env_macro,   (int)(rand01() * 56.0f) - 28);

        if (rand01() < (0.10f + amount * 0.22f)) {
            const uint8_t zone_count = 6u;
            s.zone = rand_u8(zone_count);
        }
        if (rand01() < (0.12f + amount * 0.18f)) {
            int idx = (int)quantized_time_div_index(s.time_div);
            int step = 1 + (int)(rand01() * 2.0f);
            if (rand01() < 0.5f) step = -step;
            idx += step;
            if (idx < 0) idx = 0;
            if (idx >= (int)TIME_DIV_COUNT) idx = TIME_DIV_COUNT - 1;
            s.time_div = TIME_DIV_STEPS[idx];
        }
        if (rand01() < (0.10f + amount * 0.20f)) {
            int root = (int)s.root + ((rand01() < 0.5f) ? -1 : 1) * (1 + (int)(rand01() * 4.0f));
            while (root < 0) root += 12;
            while (root > 11) root -= 12;
            s.root = (uint8_t)root;
        }
        if (rand01() < (0.08f + amount * 0.12f)) {
            static constexpr uint8_t controlled_scales[] = {
                (uint8_t)ScaleId::MAJOR,
                (uint8_t)ScaleId::NAT_MINOR,
                (uint8_t)ScaleId::DORIAN,
                (uint8_t)ScaleId::PENTA_MIN,
                (uint8_t)ScaleId::HIJAZ,
                (uint8_t)ScaleId::PELOG,
            };
            s.scale_id = controlled_scales[rand_u8((uint8_t)(sizeof(controlled_scales) / sizeof(controlled_scales[0])))];
        }
        s.seed ^= rng_next();
        if (s.seed == 0) s.seed = generate_seed(active_slot_) ^ rng_next();
    }

    // Sanitización final del mutate.
    sanitize_snapshot(s, active_slot_);

    ctx_.zone         = s.zone;
    ctx_.macro        = s.macro;
    ctx_.tonal        = s.tonal;
    ctx_.time_div     = s.time_div;
    ctx_.spread       = s.spread;
    ctx_.quant_amount = Quantizer::pot_to_amount(s.tonal);

    reverb_room_live_ = s.reverb_room;
    reverb_wet_live_  = s.reverb_wet;
    delay_div_live_   = s.delay_div;
    delay_wet_live_   = s.delay_wet;
    drive_live_       = s.drive;
    glide_live_       = s.glide_time;
    scale_id_live_    = s.scale_id;
    root_live_        = s.root;
    encoder_.scale_id = s.scale_id;
    encoder_.root     = s.root;
    drum_color_live_  = s.drum_color;
    drum_decay_live_  = s.drum_decay;
    env_release_live_ = s.env_release;
    env_attack_live_  = s.env_attack;
    env_loop_live_    = s.env_loop;

    hp_live_     = s.filter_cutoff;
    chorus_live_ = s.fx_amount;
    grain_live_  = jitter_in_range(grain_live_, amount, wild ? 0.12f : 0.04f, 0.0f, 0.55f, rand01());
    snap_live_   = jitter_in_range(snap_live_, amount, wild ? 0.10f : 0.03f, 0.0f, 0.45f, rand01());

    graphs_[active_graph_].generate(s.seed, s.zone, make_zone(s.zone));
    apply_snapshot_engine_to_graph(graphs_[active_graph_], s);
    if (pending_slot_ != NO_PENDING) {
        graphs_[incoming_graph_].generate(s.seed ^ 0xA5A5A5A5u, s.zone, make_zone(s.zone));
        apply_snapshot_engine_to_graph(graphs_[incoming_graph_], s);
    }

    LOG_AUDIO("STATE: mutate slot=%u amount=%.2f mode=%s seed=0x%08lX zone=%u fa=%u fb=%u morph=%u",
              active_slot_, amount, wild ? "WILD" : "SOFT",
              (unsigned long)s.seed, s.zone, s.formula_a, s.formula_b, s.morph);
}

void StateManager::randomize_all(RandomizeMode mode) {
    const bool wild = (mode == RandomizeMode::WILD);

    random_state_ ^= (uint32_t)to_ms_since_boot(get_absolute_time());
    random_state_ ^= ((uint32_t)active_slot_ + 1u) * 0x9E3779B9u;

    static constexpr uint8_t scale_choices[] = {
        (uint8_t)ScaleId::MAJOR,
        (uint8_t)ScaleId::PENTA_MIN,
        (uint8_t)ScaleId::DORIAN,
    };

    for (uint8_t i = 0; i < NUM_SNAPSHOTS; ++i) {
        Snapshot& s = snapshots_[i];
        s.snapshot_version = SNAPSHOT_ENGINE_VERSION;
        s.seed       = generate_seed(i) ^ rng_next();
        s.zone       = wild ? rand_u8(6) : (uint8_t)(2 + rand_u8(3));
        s.macro      = wild ? rand01() : rand_range(0.15f, 0.85f);
        s.glide_time = wild ? rand_range(0.0f, 2.0f) : rand_range(0.05f, 0.90f);
        s.time_div   = TIME_DIV_STEPS[ wild ? rand_u8(TIME_DIV_COUNT)
                                            : (uint8_t)(1 + rand_u8(TIME_DIV_COUNT - 2)) ];
        s.tonal      = wild ? rand01() : rand_range(0.25f, 0.90f);
        s.spread     = wild ? rand01() : rand_range(0.05f, 0.55f);
        s.filter_cutoff = rand01();
        s.fx_amount     = wild ? rand01() : rand_range(0.1f, 0.7f);
        s.drive      = wild ? rand_range(0.0f, 1.0f) : rand_range(0.05f, 0.55f);
        s.reverb_room = wild ? rand01() : rand_range(0.45f, 0.92f);
        s.reverb_wet  = wild ? rand_range(0.0f, 0.9f) : rand_range(0.08f, 0.42f);
        s.scale_id    = scale_choices[rand_u8((uint8_t)(sizeof(scale_choices) / sizeof(scale_choices[0])))];
        s.root        = wild ? rand_u8(12) : Quantizer::FAVORITE_ROOTS[rand_u8(5)];
        s.drum_color  = wild ? rand01() : rand_range(0.10f, 0.75f);
        s.drum_decay  = wild ? rand01() : rand_range(0.25f, 0.80f);
        s.env_release = wild ? rand01() : rand_range(0.02f, 0.45f);
        s.env_attack  = wild ? rand_range(0.0f, 0.7f) : rand_range(0.0f, 0.25f);
        s.env_loop    = wild ? (rand01() > 0.55f) : (rand01() > 0.82f);
        // V1.19: delay en snapshot
        s.delay_div   = wild ? rand01() : rand_range(0.10f, 0.55f);
        s.delay_wet   = wild ? rand_range(0.0f, 0.6f) : rand_range(0.0f, 0.25f);
        s.formula_a   = rand_u8(BytebeatGraph::FORMULA_COUNT);
        s.formula_b   = rand_u8(BytebeatGraph::FORMULA_COUNT);
        s.morph       = wild ? (uint8_t)rng_next() : (uint8_t)(48 + rand_u8(112));
        s.rate        = wild ? (uint8_t)rng_next() : (uint8_t)(80 + rand_u8(120));
        s.shift       = rand_u8(8);
        s.mask        = (uint8_t)rng_next();
        s.feedback    = wild ? (uint8_t)rng_next() : rand_u8(96);
        s.jitter      = wild ? (uint8_t)rng_next() : rand_u8(64);
        s.phase       = (uint8_t)rng_next();
        s.xor_fold    = wild ? (uint8_t)rng_next() : rand_u8(86);
        s.bb_seed     = (uint8_t)rng_next();
        s.filter_macro = wild ? (uint8_t)rng_next() : (uint8_t)(96 + rand_u8(96));
        s.resonance   = wild ? rand_u8(180) : rand_u8(96);
        s.env_macro   = (uint8_t)rng_next();
        s.valid       = true;
    }

    const Snapshot& a = snapshots_[active_slot_];
    ctx_.zone     = a.zone;
    ctx_.macro    = a.macro;
    ctx_.tonal    = a.tonal;
    ctx_.time_div = a.time_div;
    ctx_.spread   = a.spread;

    reverb_room_live_   = a.reverb_room;
    reverb_wet_live_    = a.reverb_wet;
    drive_live_         = a.drive;
    glide_live_         = a.glide_time;
    scale_id_live_      = a.scale_id;
    root_live_          = a.root;
    encoder_.scale_id   = a.scale_id;
    encoder_.root       = a.root;
    drum_color_live_    = a.drum_color;
    drum_decay_live_    = a.drum_decay;
    env_release_live_   = a.env_release;
    env_attack_live_    = a.env_attack;
    env_loop_live_      = a.env_loop;
    delay_div_live_     = a.delay_div;  // V1.19
    delay_wet_live_     = a.delay_wet;
    env_loop_time_live_ = wild ? rand_range(0.2f, 1.0f) : rand_range(0.55f, 1.0f);
    chorus_live_        = wild ? rand01() : rand_range(0.0f, 0.45f);
    hp_live_            = wild ? rand01() : rand_range(0.0f, 0.35f);
    grain_live_         = wild ? rand01() : rand_range(0.0f, 0.40f);
    snap_live_          = wild ? rand01() : rand_range(0.0f, 0.35f);

    graphs_[active_graph_].generate(a.seed, a.zone, make_zone(a.zone));
    apply_snapshot_engine_to_graph(graphs_[active_graph_], a);
    if (pending_slot_ != NO_PENDING) {
        graphs_[incoming_graph_].generate(a.seed ^ 0xA5A5A5A5u, a.zone, make_zone(a.zone));
        apply_snapshot_engine_to_graph(graphs_[incoming_graph_], a);
    }

    LOG_AUDIO("STATE: randomize_all mode=%s slot=%u seed=0x%08lX",
              wild ? "WILD" : "CTRL",
              active_slot_,
              (unsigned long)a.seed);
}


// ── Quantizer control ─────────────────────────────────────────
void StateManager::set_scale(uint8_t sid) {
    if (sid < (uint8_t)ScaleId::NUM_SCALES) {
        begin_live_write();
        scale_id_live_    = sid;
        encoder_.scale_id = sid;
        end_live_write();
    }
}

void StateManager::set_root(uint8_t r) {
    if (r < 12) {
        begin_live_write();
        root_live_     = r;
        encoder_.root  = r;
        end_live_write();
    }
}

void StateManager::next_encoder_mode() {
    uint8_t m = static_cast<uint8_t>(encoder_.mode);
    m = static_cast<uint8_t>((m + 1u) % static_cast<uint8_t>(EncoderMode::COUNT));
    encoder_.mode = static_cast<EncoderMode>(m);
}

void StateManager::encoder_delta(int delta, bool shifted) {
    if (delta == 0) return;

    switch (encoder_.mode) {
    case EncoderMode::BPM: {
        const int step = shifted ? 1 : (delta > 0 ? 1 : -1);
        int bpm = static_cast<int>(encoder_.bpm) + step;
        if (bpm < 20) bpm = 20;
        if (bpm > 240) bpm = 240;
        encoder_.bpm = static_cast<uint16_t>(bpm);
        break;
    }

    case EncoderMode::SWING: {
        const float step = shifted ? 0.005f : 0.02f;
        float s = encoder_.swing_amount + (delta > 0 ? step : -step);
        if (s < 0.0f) s = 0.0f;
        if (s > 1.0f) s = 1.0f;
        encoder_.swing_amount = s;
        break;
    }

    case EncoderMode::ROOT: {
        const int step = shifted ? 7 : 1;
        int r = static_cast<int>(root_live_) + (delta > 0 ? step : -step);
        while (r < 0)  r += 12;
        while (r > 11) r -= 12;
        set_root(static_cast<uint8_t>(r));
        break;
    }

    case EncoderMode::SCALE: {
        const int step = shifted ? 2 : 1;
        int s = static_cast<int>(scale_id_live_) + (delta > 0 ? step : -step);
        if (s < 0) s = 0;
        if (s >= static_cast<int>(ScaleId::NUM_SCALES)) s = static_cast<int>(ScaleId::NUM_SCALES) - 1;
        set_scale(static_cast<uint8_t>(s));
        break;
    }

    case EncoderMode::MUTATE: {
        const float step = shifted ? 0.01f : 0.05f;
        float m = encoder_.mutate_amount + (delta > 0 ? step : -step);
        if (m < 0.0f) m = 0.0f;
        if (m > 1.0f) m = 1.0f;
        encoder_.mutate_amount = m;
        break;
    }

    case EncoderMode::DENSITY: {
        const float step = shifted ? 0.01f : 0.05f;
        float m = encoder_.density_amount + (delta > 0 ? step : -step);
        if (m < 0.0f) m = 0.0f;
        if (m > 1.0f) m = 1.0f;
        encoder_.density_amount = m;
        break;
    }

    case EncoderMode::CHAOS: {
        const float step = shifted ? 0.01f : 0.05f;
        float m = encoder_.chaos_amount + (delta > 0 ? step : -step);
        if (m < 0.0f) m = 0.0f;
        if (m > 1.0f) m = 1.0f;
        encoder_.chaos_amount = m;
        break;
    }

    case EncoderMode::SPACE: {
        const float step = shifted ? 0.01f : 0.05f;
        float m = encoder_.space_amount + (delta > 0 ? step : -step);
        if (m < 0.0f) m = 0.0f;
        if (m > 1.0f) m = 1.0f;
        encoder_.space_amount = m;
        apply_space_macro();
        break;
    }

    default:
        break;
    }
}

void StateManager::home_reset(HomeLevel level)
{
    begin_live_write();
    // ── Nivel 1 (SOFT): siempre se aplica ──────────────────────
    // Encoder de vuelta a BPM (modo más seguro en live)
    encoder_.mode = EncoderMode::BPM;

    // Bus params → valores del snapshot activo actual
    const Snapshot& s = snapshots_[active_slot_];
    if (s.valid) {
        reverb_room_live_ = s.reverb_room;
        reverb_wet_live_  = s.reverb_wet;
        drive_live_       = s.drive;
        // Chorus/HP sí pueden quedar en snapshot; restaurarlos evita
        // inconsistencias entre el sonido guardado y el HOME reset.
        chorus_live_      = s.fx_amount;
        hp_live_          = s.filter_cutoff;
        glide_live_       = s.glide_time;
    }

    // Delay div+wet → del snapshot activo (persisten en snapshot desde V1.18).
    // fb siempre a 40% (bus global performático, no en snapshot).
    if (s.valid) {
        delay_div_live_ = s.delay_div;
        delay_wet_live_ = s.delay_wet;
    }
    delay_fb_live_ = 0.44f;  // fb: bus global, no en snapshot

    // snap gate → a cero (efecto extremo, mejor empezar limpio)
    snap_live_ = 0.0f;
    encoder_.density_amount = 0.50f;
    encoder_.chaos_amount   = 0.25f;
    if (level == HomeLevel::SOFT) {
        // HOME reset no debe sobreescribir inmediatamente el carácter espacial
        // recordado por el snapshot activo; solo restablece el estado global
        // de los encoders para la próxima intervención performática.
    }

    if (level == HomeLevel::FULL) {
        // ── Nivel 2 (FULL): mutes, drum params, env, beat repeat ─
        mute_kick_  = false;
        mute_snare_ = false;
        mute_hat_   = false;
        if (s.valid) {
            drum_color_live_  = s.drum_color;
            drum_decay_live_  = s.drum_decay;
            env_attack_live_  = s.env_attack;
            env_release_live_ = s.env_release;
        }
        // Beat repeat div → default 1/8 (zona 1 de 4)
        beat_repeat_div_live_ = 0.33f;
        // duck → off
        duck_amount_live_ = 0.0f;
    }

    LOG_AUDIO("STATE: home_reset level=%u slot=%u", (uint8_t)level, active_slot_);
    end_live_write();
}


uint32_t StateManager::derive_seed_from_variation(float v) const {
    const float c = clamp01(v);
    const uint32_t lane = (uint32_t)(c * 65535.0f + 0.5f);
    return snapshots_[active_slot_].seed ^ (lane * 0x45D9F3Bu) ^ ((uint32_t)ctx_.zone * 0x9E3779B9u);
}

void StateManager::rebuild_active_graph(uint32_t seed, uint8_t zone) {
    graphs_[active_graph_].generate(seed, zone, make_zone(zone));
    apply_snapshot_engine_to_graph(graphs_[active_graph_], snapshots_[active_slot_]);
    if (pending_slot_ != NO_PENDING) {
        graphs_[incoming_graph_].generate(seed ^ 0xA5A5A5A5u, zone, make_zone(zone));
        apply_snapshot_engine_to_graph(graphs_[incoming_graph_], snapshots_[active_slot_]);
    }
}

void StateManager::set_zone_live(uint8_t zone) {
    if (zone > 4) zone = 4;
    begin_live_write();
    ctx_.zone = zone;
    snapshots_[active_slot_].zone = zone;
    rebuild_active_graph(derive_seed_from_variation(seed_variation_live_), zone);
    end_live_write();
}

void StateManager::set_seed_variation_live(float v) {
    begin_live_write();
    seed_variation_live_ = clamp01(v);
    const uint32_t seed = derive_seed_from_variation(seed_variation_live_);
    snapshots_[active_slot_].seed = seed;
    rebuild_active_graph(seed, ctx_.zone);
    end_live_write();
}


AudioSnapshot StateManager::make_audio_snapshot() const {
    AudioSnapshot snap = {};
    for (;;) {
        const uint32_t seq_a = live_state_seq_;
        compiler_barrier();
        if (seq_a & 1u) continue;

        snap.ctx = ctx_;
        snap.reverb_room = reverb_room_live_;
        snap.reverb_wet = reverb_wet_live_;
        snap.drive = drive_live_;
        snap.glide = glide_live_;
        snap.scale_id = scale_id_live_;
        snap.root = root_live_;
        snap.drum_color = drum_color_live_;
        snap.drum_decay = drum_decay_live_;
        snap.duck_amount = duck_amount_live_;
        snap.chorus = chorus_live_;
        snap.hp = hp_live_;
        snap.grain = grain_live_;
        snap.snap = snap_live_;
        snap.delay_div = delay_div_live_;
        snap.delay_fb = delay_fb_live_;
        snap.delay_wet = delay_wet_live_;
        snap.beat_repeat_div = beat_repeat_div_live_;
        snap.env_release = env_release_live_;
        snap.env_attack = env_attack_live_;
        snap.env_loop = env_loop_live_;
        snap.env_loop_time = env_loop_time_live_;
        snap.bpm = encoder_.bpm;
        snap.note_mode_active = note_mode_active_;
        snap.note_pitch_ratio = note_pitch_ratio_;
        snap.note_active = note_active_;
        snap.note_degree = note_degree_;
        snap.note_midi = note_midi_;
        snap.note_voice_source = note_voice_source_;
        snap.current_step_index = current_step_index_;
        snap.transport_running = transport_running_;
        compiler_barrier();
        const uint32_t seq_b = live_state_seq_;
        if (seq_a == seq_b && !(seq_b & 1u)) break;
    }
    return snap;
}

uint8_t StateManager::get_current_step_index() const {
    compiler_barrier();
    return current_step_index_;
}

bool StateManager::is_transport_running() const {
    compiler_barrier();
    return transport_running_;
}

void StateManager::set_transport_running(bool running) {
    begin_live_write();
    transport_running_ = running;
    end_live_write();
}

void StateManager::set_current_step_index(uint8_t step) {
    begin_live_write();
    current_step_index_ = step;
    end_live_write();
}
