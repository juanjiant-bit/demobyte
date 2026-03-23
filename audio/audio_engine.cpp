#include "../synth/quantizer.h"
#include "audio_engine.h"
#include "dsp/fast_trig.h"
#include "operators.h"
#include "float_ops.h"
#include "floatbeat_seed.h"
#include "../state/state_manager.h"
#include "../synth/bytebeat_node.h"
#include "../dsp/dsp_chain.h"
#include "pico/stdlib.h"
#include <algorithm>
#include <cmath>


static const int bass_patterns[4][8] = {
    {0,2,4,2,0,2,4,5},
    {0,3,5,3,0,3,5,6},
    {0,2,3,5,7,5,3,2},
    {0,5,3,2,0,6,5,3}
};


// Lightweight fallback bass quantizer used until full Quantizer integration.
static int quantize_bass_degree(int degree, uint8_t scale_id) {
    // simple 7-note major/minor fallback (lightweight)
    static const int major[7] = {0,2,4,5,7,9,11};
    static const int minor[7] = {0,2,3,5,7,8,10};

    const int* scale = (scale_id == 0) ? major : minor;
    return scale[degree % 7];
}




void AudioEngine::update_bass_movement() {
    // Bass timing prefers transport-derived step data when available.
    // If transport state is invalid, fall back to BPM-derived stepping.
    // Stage 10L:
    // Prefer transport-coupled stepping using the sequencer's current step when available.
    // Fallback to the BPM-derived accumulator if the step does not advance.
    const uint8_t seq_step = control_snapshot_.current_step_index;
    const bool transport_running = control_snapshot_.transport_running;

    bool advanced = false;

    const bool seq_step_valid = (seq_step != 0xFFu);

    if (bass_transport_lock_ && transport_running && seq_step_valid) {
        if (seq_step != bass_last_seq_step_) {
            bass_last_seq_step_ = seq_step;
            bass_seq_step_ = (uint8_t)(seq_step & 7u);
            bass_seq_accum_ = 0u;
            advanced = true;
        }
    } else {
        bass_last_seq_step_ = 0xFFu;
    }

    if (!advanced) {
        const float bpm = control_snapshot_.bpm;
        const float steps_per_beat = 2.0f;   // 1/8 notes fallback
        const float steps_per_second = (bpm / 60.0f) * steps_per_beat;
        uint32_t inc = (uint32_t)(steps_per_second * 65536.0f / 44100.0f * 32.0f);
        if (inc < 1u) inc = 1u;
        if (inc > 8192u) inc = 8192u;

        bass_seq_accum_ += inc;
        if (bass_seq_accum_ >= 65536u) {
            bass_seq_accum_ -= 65536u;
            bass_seq_step_ = (uint8_t)((bass_seq_step_ + 1u) & 7u);
            advanced = true;
        }
    }

    if (!advanced) return;

    // Pattern changes happen on phrase boundaries for stability.
    if ((bass_seq_step_ & 7u) == 0u) {
        bass_pattern_id_ = (uint8_t)(macro_motion_.chaos_s * 3.9f);
        if (bass_pattern_id_ > 3u) bass_pattern_id_ = 3u;
    }

    const int raw_degree = bass_patterns[bass_pattern_id_][bass_seq_step_ & 7u] % 7;
    const int quant = quantize_bass_degree(raw_degree, cached_ctx_.scale_id);

    int base = (quant + cached_ctx_.root) % 12;
    const int octave_jump =
        (macro_motion_.rhythm_s > 0.72f && breath_amount_ > 0.56f) ? 12 : 0;

    bass_note_offset_ = base + octave_jump;
    if (bass_note_offset_ < 0) bass_note_offset_ = 0;
    if (bass_note_offset_ > 24) bass_note_offset_ = 24;
}




static const float bass_ratio_table[12] = {
    1.0f, 1.05946f, 1.12246f, 1.18921f,
    1.25992f, 1.33484f, 1.41421f, 1.49831f,
    1.58740f, 1.68179f, 1.78180f, 1.88775f
};

inline float clamp01(float x){ return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
inline float clamp_nonneg(float x){ return x < 0.f ? 0.f : x; }
// audio_engine.cpp — Bytebeat Machine V1.21 / Build v42
//
// Cambios vigentes:
//   + control-rate cacheado por bloque
//   + envelope evaluado una sola vez por sample
//   + swing real en secuenciador
//   + mutate snapshot + secuencia
//   + spread estéreo optimizado por segmentos para evitar doble evaluación
//     completa del graph en cada sample

namespace {
static uint16_t note_env_step_from_macro(uint8_t env_macro, bool note_off_release = false) {
    // 0..255 -> SWELL / PLUCK / GATE / ACCENT / TAIL
    if (note_off_release) {
        if (env_macro < 51u)  return 1400u; // swell release
        if (env_macro < 102u) return 5200u; // pluck release faster
        if (env_macro < 153u) return 7600u; // gate release fastest
        if (env_macro < 204u) return 4200u; // accent medium-fast
        return 1800u;                       // tail long release
    } else {
        if (env_macro < 51u)  return 720u;   // swell
        if (env_macro < 102u) return 2600u;  // pluck
        if (env_macro < 153u) return 5200u;  // gate
        if (env_macro < 204u) return 1800u;  // accent
        return 560u;                         // tail
    }
}
}

static inline float wrap_phase01(float x) {
    while (x >= 1.0f) x -= 1.0f;
    while (x < 0.0f) x += 1.0f;
    return x;
}

static inline float voice_shaper(float x, float drive, float tone) {
    float y = x * (1.0f + drive);
    y = y / (1.0f + fabsf(y));

    float low = y * (1.0f - tone);
    float high = (y - low) * (1.0f + tone * 0.8f);
    return low + high;
}

static inline float process_voice_comb(float x, float* buf, uint16_t& idx, float& lp) {
    const float delayed = buf[idx];
    lp += 0.18f * (delayed - lp);
    float out = x + lp * 0.42f;
    const float fb = delayed * 0.64f;
    buf[idx] = x + fb;
    idx = (uint16_t)((idx + 1u) & 255u);
    // keep the comb musical and bounded
    out = out / (1.0f + 0.25f * fabsf(out));
    update_breath_analysis(fabsf(out));
    return out;
}


static uint16_t snapshot_env_step_from_release(float env_release, uint8_t env_macro, bool note_off_release = false) {
    const float r = (env_release < 0.0f) ? 0.0f : ((env_release > 1.0f) ? 1.0f : env_release);
    if (note_off_release) {
        if (r > 0.97f) return 0u;
        if (r > 0.75f) return 900u;
        if (r > 0.50f) return 2200u;
        if (r > 0.25f) return 4200u;
        return note_env_step_from_macro(env_macro, true);
    }
    if (r > 0.97f) return 0u;
    if (r > 0.75f) return 600u;
    if (r > 0.50f) return 1500u;
    if (r > 0.25f) return 3200u;
    return note_env_step_from_macro(env_macro, false);
}



static inline uint8_t note_operator_algo_from_env(uint8_t env_macro) {
    if (env_macro < 64u)  return 0u; // pluck FM
    if (env_macro < 128u) return 1u; // metallic ring
    if (env_macro < 192u) return 2u; // sub bass
    return 3u;                       // noisy resonant
}

static inline uint8_t snapshot_operator_algo_from_env(uint8_t env_macro) {
    if (env_macro < 64u)  return 0u; // pad FM
    if (env_macro < 128u) return 2u; // rooted bass/pulse
    if (env_macro < 192u) return 1u; // metallic motion
    return 3u;                       // airy noisy texture
}

static AudioEngine* g_engine = nullptr;

void AudioEngine::init(AudioOutput* output, StateManager* state) {
    output_    = output;
    state_mgr_ = state;
    output_->init();
    dsp_.init();
    drums_.init();
    lead_osc_.init();
    envelope_.init();
    if (state_mgr_) state_mgr_->fill_context(cached_ctx_);
    global_time_ = 0.0f;
    slow_phase_a_ = 0.0f;
    slow_phase_b_ = 0.0f;
    snapshot_drift_cached_ = 0.0f;
    snapshot_air_cached_ = 0.0f;
    voice_comb_idx_ = 0;
    voice_comb_lp_ = 0.0f;
    for (float &v : voice_comb_buf_) v = 0.0f;
    // v42: arranque con el snapshot base respirando; no inicia en silencio.
    env_gate_ = true;
    env_gate_hold_ctr_ = 0;
    envelope_.retrigger();
    g_engine = this;
}

void AudioEngine::set_event_queue(RingBuffer<SequencerEvent, 128>* q) {
    event_queue_ = q;
}

bool AudioEngine::timer_callback(repeating_timer_t*) {
    if (g_engine) g_engine->generate_samples();
    return true;
}

void AudioEngine::run() {
    add_repeating_timer_ms(-1, timer_callback, nullptr, &timer_);
    while (true) tight_loop_contents();
}

void AudioEngine::generate_samples() {
    accumulator_ += ACCUM_ADD;
    while (accumulator_ >= ACCUM_TOP) {
        accumulator_ -= ACCUM_TOP;
        process_one_sample();
        sample_tick_++;
    }
}

void AudioEngine::update_macro_motion(int32_t bb_i, float macro) {
    // Ruta hot-path mínima: solo acumula métricas por bloque.
    // El smoothing, gates y derivación de outputs pasan a control-rate.
    const float macro_in = clamp01(macro);
    macro_motion_.macro_last = macro_in;

    const uint32_t u = uint32_t(bb_i);
    const float rhythm_bits = float((u >> 4) & 0xFFu) * (1.0f / 255.0f);
    const float transient = clamp01(fabsf(float(bb_i - macro_motion_.prev_bb)) * (1.0f / 32768.0f));
    const float abs_norm = clamp01(fabsf(float(bb_i)) * (1.0f / 32768.0f));

    macro_motion_.rhythm_accum += rhythm_bits;
    macro_motion_.transient_accum += transient;
    macro_motion_.density_accum += abs_norm * 0.60f;
    if (macro_motion_.accum_count < 255u) {
        macro_motion_.accum_count++;
    }

    // Chaos decimado: suficiente para la modulación lenta, más barato en ISR.
    if ((++macro_motion_.chaos_decim & 0x03u) == 0u) {
        uint16_t lfsr = macro_motion_.lfsr_state;
        if (lfsr == 0) lfsr = 0xACE1u;
        lfsr ^= (uint16_t)(u & 0xFFFFu);
        const uint16_t lsb = lfsr & 1u;
        lfsr >>= 1;
        if (lsb) lfsr ^= 0xB400u;
        macro_motion_.lfsr_state = lfsr;
        macro_motion_.chaos_accum += float(lfsr) * (1.0f / 65535.0f);
        if (macro_motion_.chaos_count < 255u) {
            macro_motion_.chaos_count++;
        }
    }

    macro_motion_.prev_bb = bb_i;
}

void AudioEngine::finalize_macro_motion_block() {
    const float inv_n = (macro_motion_.accum_count > 0u) ? (1.0f / float(macro_motion_.accum_count)) : 0.0f;
    const float inv_c = (macro_motion_.chaos_count > 0u) ? (1.0f / float(macro_motion_.chaos_count)) : 0.0f;

    const float rhythm_bits = macro_motion_.rhythm_accum * inv_n;
    const float transient = macro_motion_.transient_accum * inv_n;
    macro_motion_.rhythm_raw = 0.6f * rhythm_bits + 0.4f * transient;
    macro_motion_.density_raw = macro_motion_.density_accum * inv_n;
    macro_motion_.chaos_raw = (macro_motion_.chaos_count > 0u)
        ? (macro_motion_.chaos_accum * inv_c)
        : macro_motion_.chaos_raw;

    // Coeficientes equivalentes aproximados a 32 muestras de los EMA sample-rate.
    macro_motion_.macro_s   += (macro_motion_.macro_last - macro_motion_.macro_s) * 0.275f;
    macro_motion_.rhythm_s  += (macro_motion_.rhythm_raw  - macro_motion_.rhythm_s)  * 0.931f;
    macro_motion_.chaos_s   += (macro_motion_.chaos_raw   - macro_motion_.chaos_s)   * 0.476f;
    macro_motion_.density_s += (macro_motion_.density_raw - macro_motion_.density_s) * 0.148f;

    const float m = macro_motion_.macro_s;
    const float rhythm_bi  = macro_motion_.rhythm_s * 2.0f - 1.0f;
    const float pan_depth  = 0.35f * m;
    const float pan_target = rhythm_bi * pan_depth;
    macro_motion_.pan_s += (pan_target - macro_motion_.pan_s) * 0.148f;

    const float drive_gate  = smoothstep(0.20f, 0.70f, m);
    const float chorus_gate = smoothstep(0.35f, 0.75f, m);
    const float reverb_gate = smoothstep(0.50f, 1.00f, m);
    const float grain_gate  = smoothstep(0.75f, 1.00f, m);

    macro_out_.pan        = macro_motion_.pan_s;
    macro_out_.drive_mod  = macro_motion_.density_s * drive_gate  * 0.30f;
    macro_out_.chorus_mod = macro_motion_.chaos_s   * chorus_gate * 0.25f;
    macro_out_.reverb_mod = macro_motion_.density_s * reverb_gate * 0.25f;
    macro_out_.grain_mod  = macro_motion_.chaos_s   * grain_gate  * 0.35f;

    macro_motion_.rhythm_accum = 0.0f;
    macro_motion_.transient_accum = 0.0f;
    macro_motion_.density_accum = 0.0f;
    macro_motion_.chaos_accum = 0.0f;
    macro_motion_.accum_count = 0u;
    macro_motion_.chaos_count = 0u;
}



void AudioEngine::update_modulation_bus() {
    // Macro 2 semantic zones:
    // dry harmonic -> clearer center and support
    // hybrid       -> mixed behavior with moderate fold and float interaction
    // destructive  -> stronger FM and less stable edge
    const float blend = harmonic_blend_;
    const float blend_dry_harmonic = clamp01(1.0f - blend * 2.0f);
    const float blend_hybrid = clamp01(1.0f - fabsf(blend - 0.5f) * 4.0f);
    const float blend_destructive = clamp01((blend - 0.5f) * 2.0f);

    mod_bus_.breath_amount = breath_amount_;
    mod_bus_.harmonic_blend = harmonic_blend_;

    mod_bus_.note_fm_byte =
        blend_destructive * (0.08f + 0.22f * macro_motion_.chaos_s);

    mod_bus_.note_fm_float =
        (blend_hybrid * 0.55f + blend_destructive * 0.45f) *
        (0.08f + 0.20f * float_plane_bias_);

    mod_bus_.note_fold =
        breath_amount_ * (0.05f + 0.14f * macro_motion_.chaos_s) *
        (0.35f + 0.65f * blend_hybrid + 0.45f * blend_destructive);

    mod_bus_.note_decay =
        breath_amount_ * (0.08f + 0.18f * (1.0f - macro_motion_.density_s));

    mod_bus_.bass_gate =
        breath_amount_ * (0.10f + 0.28f * macro_motion_.rhythm_s) *
        (0.75f + 0.25f * blend_hybrid);

    mod_bus_.bass_decay =
        breath_amount_ * (0.08f + 0.22f * (1.0f - macro_motion_.density_s)) *
        (0.70f + 0.30f * blend_dry_harmonic);

    mod_bus_.drum_decay =
        breath_amount_ * (0.04f + 0.10f * perf_macro_) *
        (0.80f + 0.20f * blend_hybrid);

    mod_bus_.stutter_amount =
        breath_amount_ * blend_destructive * (0.04f + 0.16f * macro_motion_.chaos_s);

    mod_bus_.harmonic_support =
        0.75f * blend_dry_harmonic + 0.20f * blend_hybrid;

    mod_bus_.note_fm_byte = clamp01(mod_bus_.note_fm_byte);
    mod_bus_.note_fm_float = clamp01(mod_bus_.note_fm_float);
    mod_bus_.note_fold = clamp01(mod_bus_.note_fold);
    mod_bus_.note_decay = clamp01(mod_bus_.note_decay);
    mod_bus_.bass_gate = clamp01(mod_bus_.bass_gate);
    mod_bus_.bass_decay = clamp01(mod_bus_.bass_decay);
    mod_bus_.drum_decay = clamp01(mod_bus_.drum_decay);
    mod_bus_.stutter_amount = clamp01(mod_bus_.stutter_amount);
    mod_bus_.harmonic_support = clamp01(mod_bus_.harmonic_support);
}


void AudioEngine::update_breath_analysis(float mono_abs) {
    if (mono_abs < 0.0f) mono_abs = -mono_abs;

    const float diff = fabsf(mono_abs - breath_analysis_.prev_abs);
    breath_analysis_.prev_abs = mono_abs;

    breath_analysis_.acc_abs += mono_abs;
    breath_analysis_.acc_diff += diff;
    breath_analysis_.count++;
}

void AudioEngine::finalize_breath_analysis_block() {
    if (breath_analysis_.count == 0u) return;

    const float inv_n = 1.0f / float(breath_analysis_.count);
    const float avg_abs = breath_analysis_.acc_abs * inv_n;
    const float avg_diff = breath_analysis_.acc_diff * inv_n;

    // Slow/fast descriptors derived directly from recent output energy.
    breath_analysis_.env += (avg_abs - breath_analysis_.env) * 0.18f;
    breath_analysis_.spike += (avg_diff - breath_analysis_.spike) * 0.24f;

    // Very lightweight "band" proxies using the same observations:
    // low_proxy follows sustained energy, high_proxy follows edge/transient content.
    breath_analysis_.low_proxy += (avg_abs - breath_analysis_.low_proxy) * 0.10f;
    breath_analysis_.high_proxy += (avg_diff - breath_analysis_.high_proxy) * 0.14f;

    if (breath_analysis_.env < 0.0f) breath_analysis_.env = 0.0f;
    if (breath_analysis_.spike < 0.0f) breath_analysis_.spike = 0.0f;
    if (breath_analysis_.low_proxy < 0.0f) breath_analysis_.low_proxy = 0.0f;
    if (breath_analysis_.high_proxy < 0.0f) breath_analysis_.high_proxy = 0.0f;

    breath_analysis_.acc_abs = 0.0f;
    breath_analysis_.acc_diff = 0.0f;
    breath_analysis_.count = 0;
}



void AudioEngine::update_dominant_decision() {
    // Pick a single dominant decision from the detected events and hold it briefly.
    const float decision_strength = breath_amount_;

    if (decision_strength < 0.25f) {
        dominant_decision_ = DominantDecision::NONE;
        decision_hold_counter_ = 0;
        return;
    }

    if (decision_hold_counter_ > 0u) {
        decision_hold_counter_--;
        return;
    }

    if (audio_decision_.energy_peak) {
        dominant_decision_ = DominantDecision::PEAK;
    } else if (audio_decision_.kick_like) {
        dominant_decision_ = DominantDecision::KICK;
    } else if (audio_decision_.snare_like) {
        dominant_decision_ = DominantDecision::SNARE;
    } else if (audio_decision_.energy_drop) {
        dominant_decision_ = DominantDecision::DROP;
    } else {
        dominant_decision_ = DominantDecision::NONE;
    }

    if (dominant_decision_ != DominantDecision::NONE) {
        decision_hold_counter_ = 3u;
    }
}


void AudioEngine::update_audio_decision_state() {
    // Convert smoothed audio descriptors into lightweight event-like states.
    const float env = breath_env_mix_;
    const float spike = breath_spike_mix_;
    const float low = breath_analysis_.low_proxy;
    const float high = breath_analysis_.high_proxy;

    if (audio_decision_.peak_hold > 0u) audio_decision_.peak_hold--;
    if (audio_decision_.drop_hold > 0u) audio_decision_.drop_hold--;

    const bool kick_like_now =
        (low > 0.16f && spike > 0.06f);

    const bool snare_like_now =
        (high > 0.05f && spike > 0.07f);

    const bool energy_peak_now =
        (env > 0.16f && audio_decision_.prev_env <= 0.14f && audio_decision_.peak_hold == 0u);

    const bool energy_drop_now =
        (env < 0.07f && audio_decision_.prev_env >= 0.09f && audio_decision_.drop_hold == 0u);

    audio_decision_.kick_like = kick_like_now;
    audio_decision_.snare_like = snare_like_now;
    audio_decision_.energy_peak = energy_peak_now;
    audio_decision_.energy_drop = energy_drop_now;

    if (energy_peak_now) audio_decision_.peak_hold = 4u;
    if (energy_drop_now) audio_decision_.drop_hold = 4u;

    audio_decision_.prev_env = env;
}

void AudioEngine::refresh_control_block() {
    finalize_breath_analysis_block();
    update_audio_decision_state();
    update_dominant_decision();
    {
        float gate_target = 0.0f;
        float stutter_target = 0.0f;
        if (dominant_decision_ == DominantDecision::KICK) gate_target += 0.18f;
        if (dominant_decision_ == DominantDecision::PEAK) gate_target += 0.14f;
        if (dominant_decision_ == DominantDecision::SNARE) stutter_target += 0.12f;
        if (dominant_decision_ == DominantDecision::PEAK) stutter_target += 0.16f;
        if (dominant_decision_ == DominantDecision::DROP) stutter_target *= 0.4f;
        fx_gate_boost_ += (gate_target - fx_gate_boost_) * 0.18f;
        fx_stutter_boost_ += (stutter_target - fx_stutter_boost_) * 0.20f;
        if (fx_gate_boost_ < 0.0f) fx_gate_boost_ = 0.0f;
        if (fx_stutter_boost_ < 0.0f) fx_stutter_boost_ = 0.0f;
        if (fx_gate_boost_ > 0.5f) fx_gate_boost_ = 0.5f;
        if (fx_stutter_boost_ > 0.5f) fx_stutter_boost_ = 0.5f;
    }
    if (!state_mgr_) return;

    finalize_macro_motion_block();
    control_snapshot_ = state_mgr_->make_audio_snapshot();
    cached_ctx_ = control_snapshot_.ctx;

    base_drive_ = control_snapshot_.drive;
    base_reverb_room_ = control_snapshot_.reverb_room;
    base_reverb_wet_ = control_snapshot_.reverb_wet;
    base_chorus_ = control_snapshot_.chorus;
    const float drive = clamp01(base_drive_ + macro_out_.drive_mod);
    const float reverb_room = base_reverb_room_;
    // Reverb wet: base + macro_mod + aftertouch (SNAP pads)
    // El aftertouch es aditivo: presión leve = poco espacio extra,
    // presión máxima = reverb al 100% independientemente del base.
    const float reverb_wet = clamp01(base_reverb_wet_ + macro_out_.reverb_mod + at_reverb_wet_);
    const float chorus = clamp01(base_chorus_ + macro_out_.chorus_mod);
    const float hp = control_snapshot_.hp;
    base_grain_ = control_snapshot_.grain;
    // Grain wet: base + macro_mod + aftertouch (MUTE pad)
    // El aftertouch de MUTE se suma al grain base. Si fx_freeze_on_,
    // el freeze ya corre en loop → el aftertouch suma wet adicional.
    const float grain = clamp01(base_grain_ + macro_out_.grain_mod + at_grain_wet_);
    float snap = control_snapshot_.snap;
    const float bpm = control_snapshot_.bpm;
    const float beat_repeat_div = control_snapshot_.beat_repeat_div;  // 0..1 → div selector
    const float env_attack = control_snapshot_.env_attack;
    const float env_release = control_snapshot_.env_release;
    const float env_loop_time = control_snapshot_.env_loop_time;
    const float drum_color = control_snapshot_.drum_color;
    float drum_decay = control_snapshot_.drum_decay;
    const float duck = control_snapshot_.duck_amount;
    const float decision_gate_bias = 1.0f + fx_gate_boost_ * 0.08f;
    drum_decay = clamp01(drum_decay + mod_bus_.drum_decay * 0.25f);
    const float breath_kick_bias = 0.94f + 0.10f * breath_env_mix_ + 0.08f * breath_spike_mix_;
    snap = clamp01(snap + mod_bus_.stutter_amount * 0.02f + fx_stutter_boost_ * 0.03f);

    auto changed = [](float a, float b) -> bool { return (a < 0.0f) || (fabsf(a - b) > 0.0005f); };

    const float block_dt = (float)BLOCK_SIZE * (1.0f / 44100.0f);
    global_time_ += block_dt;
    slow_phase_a_ = clamp01(slow_phase_a_ + block_dt * (0.31f / 6.2831853f));
    slow_phase_b_ = clamp01(slow_phase_b_ + block_dt * (0.17f / 6.2831853f));
    if (slow_phase_a_ >= 1.0f) slow_phase_a_ -= 1.0f;
    if (slow_phase_b_ >= 1.0f) slow_phase_b_ -= 1.0f;
    snapshot_drift_cached_ = fast_sine01(slow_phase_a_) * 0.010f;
    snapshot_air_cached_   = fast_sine01(slow_phase_b_) * 0.020f;

    // Stage 6B: reconciliación de macros internos y control performativo.
    const float macro_ctrl = cached_ctx_.macro;
    const float tonal_ctrl = cached_ctx_.tonal;
    const float morph_ctrl_live = state_mgr_->get_param_normalized(PARAM_MORPH);

    perf_macro_ += (macro_ctrl - perf_macro_) * 0.12f;

    // Stage 10A:
    // P5 / PARAM_FILTER_MACRO  -> breath_amount (amount of autonomous movement)
    // P6 / PARAM_MORPH         -> harmonic_blend (dry harmonic -> hybrid -> destructive)
    breath_amount_ += ((macro_ctrl * macro_ctrl) - breath_amount_) * 0.14f;
    harmonic_blend_ += (morph_ctrl_live - harmonic_blend_) * 0.12f;
    breath_amount_ = clamp01(breath_amount_);
    harmonic_blend_ = clamp01(harmonic_blend_);
    note_fm_mix_ += (clamp01((harmonic_blend_ - 0.48f) * 1.9f) - note_fm_mix_) * 0.14f;
    note_harmonic_mix_ += (clamp01(1.0f - harmonic_blend_ * 1.8f) - note_harmonic_mix_) * 0.14f;
    note_clarity_mix_ += (clamp01(1.0f - note_fm_mix_ * 1.1f) - note_clarity_mix_) * 0.16f;
    note_layer_trim_ += ((0.92f + 0.08f * note_clarity_mix_) - note_layer_trim_) * 0.12f;
    cached_ctx_.breath_amount = breath_amount_;
    cached_ctx_.harmonic_blend = harmonic_blend_;

    const float density_ctrl = macro_motion_.density_s;
    const float low_target =
        (0.08f + 0.82f * macro_ctrl) *
        (1.02f - 0.20f * density_ctrl);

    const float body_target =
        (0.10f + 0.82f * tonal_ctrl) *
        (0.76f + 0.24f * macro_ctrl);

    const float morph_target =
        (0.08f + 0.84f * macro_ctrl) *
        (0.26f + 0.74f * tonal_ctrl);

    low_depth_ctrl_  += (low_target  - low_depth_ctrl_)  * 0.20f;
    float_body_ctrl_ += (body_target - float_body_ctrl_) * 0.17f;
    morph_ctrl_      += (morph_target - morph_ctrl_)     * 0.14f;

    low_depth_macro_   += (low_depth_ctrl_  - low_depth_macro_)   * 0.24f;
    low_depth_macro_ = clamp01(low_depth_macro_);
    float_body_macro_  += (float_body_ctrl_ - float_body_macro_)  * 0.20f;
    float_body_macro_ = clamp01(float_body_macro_);
    layer_morph_macro_ += (morph_ctrl_      - layer_morph_macro_) * 0.18f;
    layer_morph_macro_ = clamp01(layer_morph_macro_);
    low_depth_macro_ = clamp01(low_depth_macro_);
    float_body_macro_ = clamp01(float_body_macro_);
    layer_morph_macro_ = clamp01(layer_morph_macro_);

    const float fb_target =
        (0.03f + 0.36f * morph_ctrl_) *
        (0.28f + 0.72f * float_body_ctrl_);
    floatbeat_mix_macro_ += (fb_target - floatbeat_mix_macro_) * 0.16f;
    floatbeat_mix_macro_ = clamp01(floatbeat_mix_macro_);

    float dry_target = 0.0f;
    if (perf_macro_ > 0.70f) {
        float dz = (perf_macro_ - 0.70f) * 3.3333333f;
        dz = clamp01(dz);
        // slightly gentler onset so the upper macro range stays playable
        dry_target = dz * dz * (3.0f - 2.0f * dz);
    }
    dry_target *= (0.35f + 0.65f * tonal_ctrl);
    floatbeat_dry_macro_ += (dry_target - floatbeat_dry_macro_) * 0.10f;
    floatbeat_dry_macro_ = clamp01(floatbeat_dry_macro_);

    // Stage 7C: explicit byte<->float plane bias inspired by forthbyte's separation.
    const float plane_target =
        (0.12f + 0.70f * cached_ctx_.tonal) *
        (0.35f + 0.65f * layer_morph_macro_);
    float_plane_bias_ += (plane_target - float_plane_bias_) * 0.12f;
    float_plane_bias_ = clamp01(float_plane_bias_);

    update_modulation_bus();
    update_bass_movement();
    drums_.set_low_depth(low_depth_macro_);
    // Single-pot dual filter: 0..50 = LP, 50..100 = HP, with dry zone around the center.
    dsp_.set_dual_filter_control(cached_ctx_.tonal);

    if (changed(cached_drive_, drive)) { dsp_.set_drive(drive); cached_drive_ = drive; }
    const float exciter_amt = (drive <= 0.35f) ? 0.0f : clamp01((drive - 0.35f) * 1.45f);
    dsp_.set_exciter_amount(exciter_amt);
    if (changed(cached_reverb_room_, reverb_room)) { dsp_.reverb().set_room_size(reverb_room); cached_reverb_room_ = reverb_room; }
    if (changed(cached_reverb_wet_, reverb_wet)) { dsp_.reverb().set_wet(reverb_wet); cached_reverb_wet_ = reverb_wet; }
    if (changed(cached_chorus_, chorus)) { dsp_.set_chorus_amount(chorus); cached_chorus_ = chorus; }
    if (changed(cached_hp_, hp)) { dsp_.set_hp_amount(hp); cached_hp_ = hp; }
    if (changed(cached_grain_, grain) && !fx_freeze_on_) { dsp_.set_grain_amount(grain); cached_grain_ = grain; }
    if (changed(cached_snap_, snap)) { dsp_.set_snap_amount(snap); cached_snap_ = snap; }
    if (changed(cached_bpm_for_snap_, bpm)) { dsp_.set_snap_bpm(bpm); cached_bpm_for_snap_ = bpm; }
    // beat_repeat_div: solo se usa al gate_on(), no hay control de rate continuo
    (void)beat_repeat_div;  // reservado — la división se calcula en EVT_FX_ON

    // V1.17: Delay tempo-sync
    const float delay_div = control_snapshot_.delay_div;
    const float delay_fb  = control_snapshot_.delay_fb;
    const float delay_wet = control_snapshot_.delay_wet;
    if (changed(cached_delay_div_, delay_div))  { dsp_.set_delay_div(delay_div); cached_delay_div_ = delay_div; }
    if (changed(cached_delay_fb_,  delay_fb))   { dsp_.set_delay_fb(delay_fb);   cached_delay_fb_  = delay_fb;  }
    if (changed(cached_delay_wet_, delay_wet))  { dsp_.set_delay_wet(delay_wet); cached_delay_wet_ = delay_wet; }
    // Sincronizar BPM al delay cuando cambia
    if (changed(cached_bpm_for_delay_, bpm))    { dsp_.set_delay_bpm(bpm);       cached_bpm_for_delay_ = bpm;  }

    if (changed(cached_attack_, env_attack)) { envelope_.set_attack(env_attack); cached_attack_ = env_attack; }

    // FM/AM del lead: profundidad desde macro. Solo en Note Mode tiene
    // efecto audible (fuera de Note Mode el lead no se usa).
    // Se actualiza en control-rate para no llamar set_mod_depth cada sample.
    if (cached_ctx_.note_mode_active) {
        lead_osc_.set_mod_depth(cached_ctx_.macro);
    } else {
        lead_osc_.set_mod_depth(0.0f);  // seno puro fuera de Note Mode
    }
    if (changed(cached_release_, env_release)) { envelope_.set_release(env_release); cached_release_ = env_release; }
    envelope_.set_loop(state_mgr_->get_env_loop());
    if (changed(cached_env_loop_t_, env_loop_time)) { envelope_.set_loop_time_scale(env_loop_time); cached_env_loop_t_ = env_loop_time; }

    if (changed(cached_drum_color_, drum_color) || changed(cached_drum_decay_, drum_decay) || changed(cached_duck_, duck)) {
        drums_.set_params(drum_color, drum_decay, duck);
        cached_drum_color_ = drum_color;
        cached_drum_decay_ = drum_decay;
        cached_duck_ = duck;
    }

    // v38: Note Mode ya no reemplaza el motor base; la mezcla tonal sigue obedeciendo quant_amount.
    quant_mix_q15_ = (int32_t)(cached_ctx_.quant_amount * 32767.0f);
    if (quant_mix_q15_ < 0) quant_mix_q15_ = 0;
    if (quant_mix_q15_ > 32767) quant_mix_q15_ = 32767;

    const float spread = cached_ctx_.spread;
    if (spread <= 0.01f) {
        spread_mix_q15_ = 0;
        spread_stride_ = 4;
    } else {
        // Mezcla máxima = 50% del canal R alternativo.
        spread_mix_q15_ = (uint16_t)(spread * 0.5f * 32767.0f);
        if (spread_mix_q15_ > 16384u) spread_mix_q15_ = 16384u;

        // A más spread, más resolución temporal. Reduce costo 2x-8x según uso.
        spread_stride_ = (spread > 0.66f) ? 2 : (spread > 0.33f ? 4 : 8);
        spread_phase_inc_q8_ = (uint8_t)(256 / spread_stride_);
        spread_t_offset_ = 1u + (uint32_t)(spread * 15.0f);
        spread_macro_delta_ = spread * 0.04f;
    }
}

void AudioEngine::begin_spread_segment(const EvalContext& ctx, uint32_t base_t, float pitch_ratio) {
    EvalContext ctx_now = ctx;
    ctx_now.t = (uint32_t)((float)(base_t + spread_t_offset_) * pitch_ratio);
    ctx_now.macro = ctx.macro + spread_macro_delta_;
    if (ctx_now.macro > 1.0f) ctx_now.macro = 1.0f;
    if (ctx_now.macro < 0.0f) ctx_now.macro = 0.0f;

    const uint32_t future_sample = sample_tick_ + spread_stride_;
    const uint32_t future_base_t = (uint32_t)((float)future_sample * ctx.time_div);
    EvalContext ctx_future = ctx_now;
    ctx_future.t = (uint32_t)((float)(future_base_t + spread_t_offset_) * pitch_ratio);

    // Preview sin side-effects: evita contaminar feedback/filtros internos del BytebeatGraph.
    spread_seg_start_ = state_mgr_->evaluate_preview(ctx_now);
    spread_seg_end_   = state_mgr_->evaluate_preview(ctx_future);
    spread_phase_     = 0;
}

void AudioEngine::drain_events() {
    if (!event_queue_) return;
    SequencerEvent ev;
    while (event_queue_->pop(ev)) {
        switch (ev.type) {

        case EVT_PARAM_CHANGE: {
            const ParamId pid = static_cast<ParamId>(ev.target);
            if (state_mgr_) {
                switch (pid) {
                case PARAM_REVERB_ROOM:
                case PARAM_REVERB_WET:
                case PARAM_CHORUS:
                case PARAM_DRUM_DECAY:
                case PARAM_DRUM_COLOR:
                case PARAM_DUCK_AMOUNT:
                case PARAM_DELAY_DIV:
                case PARAM_DELAY_FB:
                case PARAM_DELAY_WET:
                    state_mgr_->set_bus_param(pid, ev.value);
                    break;
                default:
                    state_mgr_->set_patch_param(pid, ev.value);
                    break;
                }
            }

            switch (pid) {
            case PARAM_DRIVE:
                dsp_.set_drive(ev.value);
                break;
            case PARAM_SNAP_GATE:
                dsp_.set_snap_amount(ev.value);
                break;
            case PARAM_ENV_ATTACK:
                envelope_.set_attack(ev.value);
                break;
            case PARAM_ENV_RELEASE:
                envelope_.set_release(ev.value);
                break;
            case PARAM_GRAIN:
                if (!fx_freeze_on_) dsp_.set_grain_amount(ev.value);
                break;
            case PARAM_BEAT_REPEAT_DIV:
                break;
            case PARAM_HP:
                dsp_.set_hp_amount(ev.value);
                break;
            case PARAM_REVERB_ROOM:
                dsp_.reverb().set_room_size(ev.value);
                break;
            case PARAM_REVERB_WET:
                dsp_.reverb().set_wet(ev.value);
                break;
            case PARAM_CHORUS:
                dsp_.set_chorus_amount(ev.value);
                break;
            case PARAM_DELAY_DIV:
                dsp_.set_delay_div(ev.value);
                cached_delay_div_ = ev.value;
                break;
            case PARAM_DELAY_FB:
                dsp_.set_delay_fb(ev.value);
                cached_delay_fb_ = ev.value;
                break;
            case PARAM_DELAY_WET:
                dsp_.set_delay_wet(ev.value);
                cached_delay_wet_ = ev.value;
                break;
            default:
                break;
            }
            break;
        }

        // V1.11: retrigger del envelope en cada trigger del sequencer
        // Esto permite que el release controle tick/pluck/pad en secuencias.
        // retrigger() NO resetea a 0 — el attack sube desde donde está
        // (legato natural, sin click en releases largos).
        case EVT_PAD_TRIGGER:
            env_gate_ = true;
            env_gate_hold_ctr_ = ENV_GATE_HOLD;  // armar el timer
            envelope_.retrigger();  // Note Mode lo cancela via EVT_NOTE_OFF si es necesario
            break;

        case EVT_DRUM_HIT:
            drums_.trigger((DrumId)ev.target);
            break;

        case EVT_ROLL_ON:
            drums_.roll_on((DrumId)ev.target);
            break;

        case EVT_ROLL_OFF:
            drums_.roll_off((DrumId)ev.target);
            break;

        case EVT_FX_ON:
            switch ((FxId)ev.target) {
            case FX_BEAT_REPEAT: {
                // División desde el pot BEAT_REPEAT_DIV (capa SHIFT P2)
                // El pot 0..1 selecciona entre 1/16, 1/8, 1/4, 1/2 en 4 zonas.
                const float bpm_now = state_mgr_->get_encoder_state().bpm;
                const float div_norm = state_mgr_->get_beat_repeat_div_live();
                const uint8_t zone = (uint8_t)(div_norm * 3.99f);  // 0..3
                // beats_x4: 1/16=0.25, 1/8=0.5, 1/4=1.0, 1/2=2.0
                static constexpr float kDivBeats[4] = { 0.25f, 0.5f, 1.0f, 2.0f };
                const float ms = (kDivBeats[zone] * 60000.f) / bpm_now;
                const uint32_t samps = (uint32_t)(ms * 44100.f / 1000.f);
                fx_beat_repeat_on_ = true;
                dsp_.stutter().gate_on(samps);
                break;
            }
            case FX_FREEZE:
                fx_freeze_on_ = true;
                dsp_.grain().force_freeze();
                break;
            case FX_OCT_DOWN:
                fx_oct_down_on_ = true;
                break;
            case FX_OCT_UP:
                fx_oct_up_on_ = true;
                break;
            case FX_VIBRATO:
                fx_vibrato_on_ = true;
                break;
            case FX_REPEAT_16: {
                // Beat repeat forzado a 1/16 (pad C, slot 0)
                const float bpm_now = state_mgr_->get_encoder_state().bpm;
                const float ms = (0.25f * 60000.f) / bpm_now;
                fx_repeat16_on_ = true;
                dsp_.stutter().gate_on((uint32_t)(ms * 44100.f / 1000.f));
                break;
            }
            case FX_REPEAT_8: {
                // Beat repeat forzado a 1/8 (pad F, slot 1)
                const float bpm_now = state_mgr_->get_encoder_state().bpm;
                const float ms = (0.5f * 60000.f) / bpm_now;
                fx_repeat8_on_ = true;
                dsp_.stutter().gate_on((uint32_t)(ms * 44100.f / 1000.f));
                break;
            }
            case FX_REPEAT_4: {
                // Beat repeat forzado a 1/4 (pad A, slot 2)
                const float bpm_now = state_mgr_->get_encoder_state().bpm;
                const float ms = (1.0f * 60000.f) / bpm_now;
                fx_repeat4_on_ = true;
                dsp_.stutter().gate_on((uint32_t)(ms * 44100.f / 1000.f));
                break;
            }
            }
            break;

        case EVT_FX_OFF:
            switch ((FxId)ev.target) {
            case FX_BEAT_REPEAT:
                fx_beat_repeat_on_ = false;
                if (!fx_repeat4_on_ && !fx_repeat8_on_ && !fx_repeat16_on_)
                    dsp_.stutter().gate_off();
                break;
            case FX_FREEZE:
                fx_freeze_on_ = false;
                dsp_.grain().force_release();
                if (state_mgr_) dsp_.set_grain_amount(state_mgr_->get_grain_live());
                else dsp_.set_grain_amount(0.0f);
                break;
            case FX_OCT_DOWN:
                fx_oct_down_on_ = false;
                break;
            case FX_OCT_UP:
                fx_oct_up_on_ = false;
                break;
            case FX_VIBRATO:
                fx_vibrato_on_ = false;
                break;
            case FX_REPEAT_4:
                fx_repeat4_on_ = false;
                if (!fx_beat_repeat_on_ && !fx_repeat8_on_ && !fx_repeat16_on_)
                    dsp_.stutter().gate_off();
                break;
            case FX_REPEAT_8:
                fx_repeat8_on_ = false;
                if (!fx_beat_repeat_on_ && !fx_repeat4_on_ && !fx_repeat16_on_)
                    dsp_.stutter().gate_off();
                break;
            case FX_REPEAT_16:
                fx_repeat16_on_ = false;
                if (!fx_beat_repeat_on_ && !fx_repeat4_on_ && !fx_repeat8_on_)
                    dsp_.stutter().gate_off();
                break;
            }
            break;

        // V1.7: drum params performáticos en vivo
        case EVT_DRUM_PARAM:
            if (state_mgr_) {
                if (ev.target == DRUM_PARAM_COLOR) state_mgr_->set_bus_param(PARAM_DRUM_COLOR, ev.value);
                else if (ev.target == DRUM_PARAM_DECAY) state_mgr_->set_bus_param(PARAM_DRUM_DECAY, ev.value);
                else if (ev.target == DRUM_PARAM_DUCK) state_mgr_->set_bus_param(PARAM_DUCK_AMOUNT, ev.value);
            }
            if (ev.target == DRUM_PARAM_COLOR) {
                drums_.set_params(ev.value, -1.0f, -1.0f);  // solo color
            } else if (ev.target == DRUM_PARAM_DECAY) {
                drums_.set_params(-1.0f, ev.value, -1.0f);  // solo decay
            } else if (ev.target == DRUM_PARAM_DUCK) {
                drums_.set_params(-1.0f, -1.0f, ev.value);  // solo ducking
            }
            break;

        // V1.7: Note Mode — aplicar pitch ratio al bytebeat
        case EVT_NOTE_ON: {
            const float ratio = NoteMode::midi_to_pitch_ratio(ev.target);
            if (state_mgr_) {
                // Compatibilidad con lead osc / OLED / telemetría de nota.
                state_mgr_->set_note_pitch(ratio);
                state_mgr_->prepare_note_voice_graph(note_voice_graph_, state_mgr_->note_voice_source()); // v40: source seleccionable
            }
            note_pitch_ratio_ = ratio;
            note_voice_.active = true;
            note_voice_.degree = ev.target;
            note_voice_.velocity = (uint8_t)((ev.value <= 0.0f) ? 96u : ((ev.value >= 1.0f) ? 127u : (uint8_t)(ev.value * 127.0f)));
            note_voice_.pitch_ratio = ratio;
            note_voice_.base_freq_hz = Quantizer::note_to_freq((uint8_t)ev.target);
            note_voice_.t_offset = 0u;
            note_voice_.env_q16 = 65535u;
            const uint8_t env_macro = note_voice_graph_.get_env_macro();
            note_voice_.env_step_q16 = note_env_step_from_macro(env_macro, false);
            note_voice_.formula_source = state_mgr_ ? state_mgr_->note_voice_source() : 0u;
            note_voice_.op_phase = 0.0f;
            note_voice_.op_phase2 = 0.0f;
            note_voice_.op_lp_z = 0.0f;
            note_voice_.op_slew_z = 0.0f;
            note_voice_.float_phase = 0.0f;
            note_voice_.float_phase2 = 0.0f;
            note_voice_.fb_state.t_f = 0.0f;
            note_voice_.op_env = 1.0f;
            note_voice_.op_mod_env = 1.0f;
            note_voice_.op_algo = note_operator_algo_from_env(env_macro);
            note_voice_.pitch_env = 1.0f;
            note_voice_.op_noise_state ^= ((uint32_t)ev.target << 16) ^ 0x9E3779B9u;
            env_gate_ = true;   // conserva comportamiento del lead/env legado
            break;
        }

        case EVT_NOTE_OFF:
            if (state_mgr_) {
                state_mgr_->clear_note_pitch();
                state_mgr_->clear_active_note();
            }
            note_pitch_ratio_ = 1.0f;
            // v39: release de la note voice depende del env macro heredado.
            if (note_voice_.active) {
                const uint8_t env_macro = note_voice_graph_.get_env_macro();
                note_voice_.env_step_q16 = note_env_step_from_macro(env_macro, true);
            }
            env_gate_ = false;
            break;

        case EVT_SNAPSHOT_VOICE_ON: {
            if (state_mgr_) {
                state_mgr_->prepare_snapshot_voice_graph(snapshot_voice_graph_, ev.target);
                const Snapshot* snaps = state_mgr_->get_snapshots();
                const uint8_t slot = ev.target;
                const Snapshot& s = snaps[(slot < StateManager::NUM_SNAPSHOTS && snaps[slot].valid) ? slot : state_mgr_->get_active_slot()];
                snapshot_voice_.active = true;
                snapshot_voice_.slot = slot;
                snapshot_voice_.velocity = (uint8_t)((ev.value <= 0.0f) ? 110u : ((ev.value >= 1.0f) ? 127u : (uint8_t)(ev.value * 127.0f)));
                snapshot_voice_.env_q16 = 65535u;
                snapshot_voice_.env_step_q16 = snapshot_env_step_from_release(s.env_release, s.env_macro, false);
                snapshot_voice_.latched = (s.env_release > 0.97f) || s.env_loop;
                snapshot_voice_.t_offset = 0u;
                snapshot_voice_.op_phase = 0.0f;
                snapshot_voice_.op_phase2 = 0.0f;
                snapshot_voice_.op_lp_z = 0.0f;
                snapshot_voice_.op_slew_z = 0.0f;
                snapshot_voice_.float_phase = 0.0f;
                snapshot_voice_.float_phase2 = 0.0f;
                snapshot_voice_.fb_state.t_f = 0.0f;
                snapshot_voice_.op_env = 1.0f;
                snapshot_voice_.op_mod_env = 1.0f;
                snapshot_voice_.op_algo = snapshot_operator_algo_from_env(s.env_macro);
                snapshot_voice_.op_noise_state ^= ((uint32_t)ev.target << 12) ^ 0x85EBCA6Bu;
            }
            break;
        }

        case EVT_SNAPSHOT_VOICE_OFF:
            if (snapshot_voice_.active) {
                if (snapshot_voice_.latched) {
                    snapshot_voice_.env_step_q16 = 0u;
                } else if (state_mgr_) {
                    const Snapshot* snaps = state_mgr_->get_snapshots();
                    const Snapshot& s = snaps[(snapshot_voice_.slot < StateManager::NUM_SNAPSHOTS && snaps[snapshot_voice_.slot].valid) ? snapshot_voice_.slot : state_mgr_->get_active_slot()];
                    snapshot_voice_.env_step_q16 = snapshot_env_step_from_release(s.env_release, s.env_macro, true);
                } else {
                    snapshot_voice_.env_step_q16 = 4096u;
                }
            }
            break;

        case EVT_AFTERTOUCH: {
            const float p   = ev.value;
            const uint8_t tgt = ev.target;

            if (tgt == PAD_MUTE) {
                // ── MUTE pad → grain freeze wet ──────────────────
                // La presión de la palma modula el wet del granulador.
                // Cuando fx_freeze_on_ está activo, el grain ya está
                // en loop; el aftertouch suma wet adicional para
                // profundizar el efecto de forma expresiva.
                at_grain_wet_ = p;
                // Si el freeze no estaba activo, activarlo suavemente
                // en cuanto la presión supera el umbral.
                if (p > 0.05f && !fx_freeze_on_) {
                    dsp_.set_grain_amount(clamp01(base_grain_ + p));
                } else if (p < 0.01f && !fx_freeze_on_) {
                    // Presión retirada sin freeze activo → restaurar base
                    dsp_.set_grain_amount(base_grain_);
                }
                // cached_grain_ se invalida para que refresh lo recalcule
                cached_grain_ = -1.0f;

            } else if (tgt == AT_TARGET_REVERB_SNAP) {
                // ── SNAP pads → reverb wet momentáneo ────────────
                // La presión abre el espacio: se suma al base_reverb_wet_
                // actual. Al soltar el pad (p=0.0), el reverb vuelve al base.
                // El cambio es suave porque EVT_AFTERTOUCH se emite con
                // hysteresis de 0.015 en handle_aftertouch.
                at_reverb_wet_ = p;
                // Invalidar cache para que refresh lo aplique inmediatamente
                cached_reverb_wet_ = -1.0f;

            } else {
                // Nota MIDI (Note Mode): target = número de nota MIDI.
                // La presión aumenta la profundidad FM momentáneamente
                // más allá del valor base del macro.
                // macro=0.3 + presión=1.0 → fm_depth sube hasta el máximo (0.40).
                // Permite expresividad táctil: tocar suave = seno limpio,
                // apretar = el bytebeat imprime su timbre en la nota.
                if (cached_ctx_.note_mode_active) {
                    const float base_depth = cached_ctx_.macro * 0.40f;
                    const float at_extra   = p * (0.40f - base_depth);
                    lead_osc_.set_mod_depth((base_depth + at_extra) / 0.40f);
                }
            }
            break;
        }

        default: break;
        }
    }
}

void AudioEngine::process_one_sample() {
    // Safe point cada BLOCK_SIZE samples: control-rate update.
    if ((sample_tick_ & (BLOCK_SIZE - 1)) == 0) {
        if (state_mgr_) state_mgr_->process_pending();
        drain_events();
        refresh_control_block();
    }

    if (!state_mgr_) { output_->write(0, 0); return; }


    // ── Bytebeat synth ───────────────────────────────────────
    EvalContext ctx = cached_ctx_;

    // v38: el motor base del snapshot ya no se afina con Note Mode.
    // Note Mode ahora vive en una Note Voice Layer paralela.
    float pitch_ratio = 1.0f;

    const float fx_pitch_target = fx_oct_down_on_ ? 0.5f : (fx_oct_up_on_ ? 2.0f : 1.0f);
    fx_pitch_mul_current_ += (fx_pitch_target - fx_pitch_mul_current_) * 0.08f;
    pitch_ratio *= fx_pitch_mul_current_;

    const float vibrato_target = fx_vibrato_on_ ? 1.0f : 0.0f;
    vibrato_depth_current_ += (vibrato_target - vibrato_depth_current_) * 0.04f;
    if (vibrato_depth_current_ > 0.001f) {
        extern const int16_t SINE_TABLE_256[256];
        const int16_t vib_s = SINE_TABLE_256[(uint8_t)(vibrato_phase_q24_ >> 16)];
        vibrato_phase_q24_ += VIBRATO_RATE_Q24;
        const float vib = 1.0f + ((float)vib_s / 32767.0f) * (0.018f + 0.01f * vibrato_depth_current_);
        pitch_ratio *= vib;
    }

    uint32_t base_t   = (uint32_t)((float)sample_tick_ * ctx.time_div);
    ctx.t = (uint32_t)((float)base_t * pitch_ratio);

    int16_t synth_l = state_mgr_->evaluate(ctx);
    int16_t synth_r = synth_l;

    update_macro_motion((int32_t)synth_l, ctx.macro);

    if (spread_mix_q15_ != 0) {
        // Spread optimizado: en vez de evaluar el graph del canal R en cada
        // sample, calculamos segmentos cortos y los interpolamos. El carácter
        // sigue siendo algorítmico, pero el costo cae fuerte cuando spread > 0.
        if (spread_phase_ == 0 || spread_phase_ >= spread_stride_) {
            begin_spread_segment(ctx, base_t, pitch_ratio);
        }

        const uint8_t frac_q8 = (uint8_t)(spread_phase_ * spread_phase_inc_q8_);
        const int16_t raw_r = lerp_i16(spread_seg_start_, spread_seg_end_, frac_q8);
        spread_phase_++;

        const int32_t wet = spread_mix_q15_;
        const int32_t dry = 32767 - wet;
        synth_r = (int16_t)((((int32_t)synth_l * dry) + ((int32_t)raw_r * wet)) >> 15);
    }

    // ── Note Voice Layer (v40) ───────────────────────────────
    // Voice paralela encima del paisaje bytebeat del snapshot.
    int16_t note_voice_s = 0;
    if (note_voice_.active) {
        EvalContext note_ctx = cached_ctx_;
        note_ctx.note_mode_active = true;
        note_ctx.note_pitch_ratio = note_voice_.pitch_ratio;

        const float nv_pitch_target = fx_oct_down_on_ ? 0.5f : (fx_oct_up_on_ ? 2.0f : 1.0f);
        const float nv_pitch = note_voice_.pitch_ratio * nv_pitch_target;
        note_ctx.t = (uint32_t)((float)(base_t + note_voice_.t_offset) * nv_pitch);

        int16_t raw_nv = note_voice_graph_.evaluate(note_ctx);
        const float note_env = (float)note_voice_.env_q16 / 65535.0f;

        const float dt = 1.0f / 44100.0f;
        note_voice_.pitch_env *= 0.992f;
        float op_freq = note_voice_.base_freq_hz * nv_pitch_target * (1.0f + note_voice_.pitch_env * 0.045f);
        if (op_freq < 30.0f) op_freq = 30.0f;
        if (op_freq > 2400.0f) op_freq = 2400.0f;
        note_voice_.op_phase = wrap_phase01(note_voice_.op_phase + op_freq * dt);
        note_voice_.op_phase2 = wrap_phase01(note_voice_.op_phase2 + (op_freq * 2.01f) * dt);
        note_voice_.float_phase = f_wrap01(note_voice_.float_phase + op_freq * dt);
        note_voice_.float_phase2 = f_wrap01(note_voice_.float_phase2 + (op_freq * 0.5f) * dt);

        const float osc = op_sine(note_voice_.op_phase);
        const float tri = op_tri(note_voice_.op_phase2);
        const float nz = op_noise(note_voice_.op_noise_state);
        const float macro = cached_ctx_.macro;
        note_voice_.op_mod_env *= 0.994f;

        float synth = 0.0f;
        // operator_amount kept from earlier routing design; current blend is solved by bb/op/float/fb weights.
        float algo_trim = 1.0f;
        float algo_low_weight = 0.24f;
        switch (note_voice_.op_algo) {
            default:
            case 0u: {
                const float mod = op_sine(note_voice_.op_phase2) * (0.55f * note_voice_.op_mod_env + macro * 0.25f);
                const float pm = op_pm(note_voice_.op_phase, mod, 1.8f + macro * 1.4f);
                const float ring = op_ring(pm, tri);
                synth = pm * 0.72f + ring * 0.20f + nz * 0.08f;
                synth = op_lp(synth, note_voice_.op_lp_z, 0.16f);
                synth = op_slew(synth, note_voice_.op_slew_z, 0.24f);
                synth = op_fold(synth, 0.34f + macro * 0.26f);
                synth = voice_shaper(synth, 0.46f, 0.28f + macro * 0.18f);
                algo_trim = 0.92f;
                algo_low_weight = 0.28f;
                break;
            }
            case 1u: {
                const float mod = tri * (0.42f + note_voice_.op_mod_env * 0.45f);
                const float pm = op_pm(note_voice_.op_phase, mod, 2.4f);
                const float ring = op_ring(pm, nz);
                synth = pm * 0.46f + tri * 0.18f + ring * 0.28f + nz * 0.08f;
                synth = op_lp(synth, note_voice_.op_lp_z, 0.19f);
                synth = op_slew(synth, note_voice_.op_slew_z, 0.19f);
                synth = op_fold(synth, 0.52f + macro * 0.20f);
                synth = voice_shaper(synth, 0.54f, 0.42f + macro * 0.18f);
                algo_trim = 0.82f;
                algo_low_weight = 0.10f;
                algo_low_weight = 0.12f;
                break;
            }
            case 2u: {
                const float subosc = op_sine(wrap_phase01(note_voice_.op_phase * 0.5f));
                const float pm = op_pm(note_voice_.op_phase, osc * (0.14f + macro * 0.18f), 0.55f);
                synth = pm * 0.44f + osc * 0.18f + subosc * 0.48f + nz * 0.03f;
                synth = op_lp(synth, note_voice_.op_lp_z, 0.09f);
                synth = op_slew(synth, note_voice_.op_slew_z, 0.12f);
                synth = op_fold(synth, 0.16f + macro * 0.10f);
                synth = voice_shaper(synth, 0.58f, 0.12f + macro * 0.10f);
                algo_trim = 0.88f;
                algo_low_weight = 0.56f;
                algo_low_weight = 0.78f;
                break;
            }
            case 3u: {
                const float mod = nz * (0.32f + note_voice_.op_mod_env * 0.25f);
                const float pm = op_pm(note_voice_.op_phase, mod, 1.1f);
                const float ring = op_ring(tri, nz);
                synth = pm * 0.34f + osc * 0.22f + ring * 0.20f + nz * 0.24f;
                synth = op_lp(synth, note_voice_.op_lp_z, 0.13f);
                synth = op_slew(synth, note_voice_.op_slew_z, 0.16f);
                synth = op_fold(synth, 0.44f + macro * 0.14f);
                synth = voice_shaper(synth, 0.36f, 0.46f + macro * 0.16f);
                algo_trim = 0.78f;
                algo_low_weight = 0.18f;
                break;
            }
        }

        const float low_osc = op_sine(wrap_phase01(note_voice_.op_phase * 0.5f));
        synth += low_osc * algo_low_weight * (0.10f + 0.40f * low_depth_macro_) * (0.25f + 0.75f * float_body_macro_);

        float float_voice = f_sine(note_voice_.float_phase) * 0.72f
                          + f_sine(note_voice_.float_phase2) * (0.30f + 0.28f * algo_low_weight);
        if (note_voice_.op_algo == 1u || note_voice_.op_algo == 3u) {
            float_voice += f_tri(note_voice_.float_phase * 0.5f) * 0.12f;
        }
        float_voice = f_softclip(float_voice);
        float_voice *= note_env * (0.22f + 0.58f * float_body_macro_) * (0.20f + 0.60f * algo_low_weight);

        const float op_env = op_decay(note_voice_.op_env, 0.002f);
        synth *= (op_env * algo_trim);
        const float note_fold_mod = mod_bus_.note_fold;

        const float fb = floatbeat_algo(
            note_voice_.fb_state,
            dt,
            op_freq,
            0.35f + 0.55f * algo_low_weight,
            note_voice_.op_algo
        );

        const float fb_shaped = f_softclip(
            fb * (0.55f + 0.30f * algo_low_weight) +
            f_sine(note_voice_.float_phase2) * (0.18f + 0.16f * float_body_macro_)
        ) * note_env;

        const float fm_voice = op_sine(wrap_phase01(
            note_voice_.op_phase +
            bb * mod_bus_.note_fm_byte * 0.07f +
            fb_shaped * mod_bus_.note_fm_float * 0.11f
        ));
        const float fm_shaped = voice_shaper(fm_voice, 0.22f + note_fold_mod, 0.18f + 0.10f * note_fm_mix_ + 0.06f * mod_bus_.note_decay);

        
        // Stage 4E: per-algo mix shaping
        float algo_bb_bias = 0.0f;
        float algo_float_bias = 0.0f;
        float algo_fb_bias = 0.0f;
        float algo_out_trim = 1.0f;

        switch (note_voice_.op_algo & 3u) {
            case 0u: // deep
                algo_bb_bias = -0.04f;
                algo_float_bias = 0.07f;
                algo_fb_bias = 0.05f;
                algo_out_trim = 0.96f;
                break;
            case 1u: // metallic
                algo_bb_bias = 0.08f;
                algo_float_bias = -0.05f;
                algo_fb_bias = -0.03f;
                algo_out_trim = 0.90f;
                break;
            case 2u: // sub
                algo_bb_bias = -0.06f;
                algo_float_bias = 0.09f;
                algo_fb_bias = 0.07f;
                algo_out_trim = 0.92f;
                break;
            case 3u: // noisy
                algo_bb_bias = 0.03f;
                algo_float_bias = -0.01f;
                algo_fb_bias = 0.03f;
                algo_out_trim = 0.94f;
                break;
        }

        float bb = (float)raw_nv / 32768.0f;

        float float_mix = 0.08f + 0.24f * float_body_macro_ + algo_float_bias;
        float fb_mix    = 0.02f + 0.26f * floatbeat_mix_macro_ + algo_fb_bias;
        float_mix += 0.05f * float_plane_bias_;
        fb_mix    += 0.05f * float_plane_bias_;
        if (float_mix < 0.0f) float_mix = 0.0f;
        if (fb_mix < 0.0f) fb_mix = 0.0f;

        float bb_mix = 0.54f + algo_bb_bias - 0.10f * float_plane_bias_
                     - 0.20f * float_body_macro_
                     - 0.10f * floatbeat_mix_macro_;

        if (bb_mix < 0.22f) bb_mix = 0.22f;

        float op_mix = 1.0f - bb_mix - float_mix - fb_mix;

        if (op_mix < 0.12f) {
            float deficit = 0.12f - op_mix;
            bb_mix -= deficit;
            if (bb_mix < 0.18f) bb_mix = 0.18f;
            op_mix = 1.0f - bb_mix - float_mix - fb_mix;
        }

        float morph = layer_morph_macro_ + 0.12f * perf_macro_ + 0.08f * float_body_macro_
                    + algo_morph_bias_[note_voice_.op_algo & 3u];
        morph = clamp01(morph);
        morph = morph * (0.8f + 0.2f * morph);

        float bb_morph    = clamp_nonneg(bb_mix    * (1.08f - 0.46f * morph));
        float op_morph    = clamp_nonneg(op_mix    * (0.92f + 0.12f * morph));
        float float_morph = clamp_nonneg(float_mix * (0.72f + 0.58f * morph));
        float fb_morph    = clamp_nonneg((fb_mix * (0.7f + 0.3f * morph)) * (0.56f + 0.82f * morph));

        float total_morph = bb_morph + op_morph + float_morph + fb_morph;
        if (total_morph < 0.0001f) total_morph = 1.0f;
        bb_morph    /= total_morph;
        op_morph    /= total_morph;
        float_morph /= total_morph;
        fb_morph    /= total_morph;

        float dry_amt = floatbeat_dry_macro_;
        switch (note_voice_.op_algo & 3u) {
            case 0u: dry_amt = 1.08f * (dry_amt * (0.72f + 0.28f * dry_amt)); break;
            case 1u: dry_amt = 0.88f * (dry_amt * dry_amt); break;
            case 2u: dry_amt = 1.22f * (0.55f * dry_amt + 0.45f * dry_amt * dry_amt); break;
            case 3u: dry_amt = 0.94f * (0.82f * dry_amt + 0.18f * dry_amt * dry_amt); break;
        }
        dry_amt = clamp01(dry_amt);
        dry_amt = dry_amt * dry_amt;

        float bb_dry    = clamp_nonneg(bb_morph    * (1.0f - 0.72f * dry_amt));
        float op_dry    = clamp_nonneg(op_morph    * (1.0f - 0.86f * dry_amt));
        float float_dry = clamp_nonneg(float_morph * (1.0f - 0.78f * dry_amt));
        float fb_dry    = clamp_nonneg(fb_morph    * (1.0f + 1.25f * dry_amt));

        float total_dry = bb_dry + op_dry + float_dry + fb_dry;
        if (total_dry < 0.0001f) total_dry = 1.0f;
        bb_dry    /= total_dry;
        op_dry    /= total_dry;
        float_dry /= total_dry;
        fb_dry    /= total_dry;

        float raw_lane = bb * (0.05f + 0.07f * perf_macro_);
        if ((note_voice_.op_algo & 3u) == 3u) {
            raw_lane += bb * (0.04f + 0.05f * macro_motion_.chaos_s);
        }
        raw_lane = clamp_nonneg(raw_lane);

        float note_boost = 0.0f;

        // Decision-driven articulation:
        // SNARE sharpens attack, PEAK pushes energy, DROP softens the response.
        if (dominant_decision_ == DominantDecision::SNARE) {
            note_boost += 0.15f; // transient articulation
        }
        if (dominant_decision_ == DominantDecision::PEAK) {
            note_boost += 0.20f; // push expression
        }
        if (dominant_decision_ == DominantDecision::DROP) {
            note_boost *= 0.5f; // soften
        }

        note_dynamic_boost_ += (note_boost - note_dynamic_boost_) * 0.2f;

        const float harmonic_layer = op_sine(wrap_phase01(note_voice_.op_phase * 1.5f)) * (0.06f * mod_bus_.harmonic_support * (0.9f + note_dynamic_boost_));

        float combined =
            bb          * bb_dry +
            synth       * op_dry +
            float_voice * float_dry +
            fb_shaped   * fb_dry +
            raw_lane * 0.10f +
            fm_shaped * ((0.04f + 0.18f * note_fm_mix_) * (0.92f + 0.14f * breath_spike_mix_) * (0.9f + note_dynamic_boost_)) +
            harmonic_layer * note_harmonic_mix_;
        combined *= note_layer_trim_;
        combined *= algo_out_trim;
        combined *= note_env;
        combined *= (float)note_voice_.velocity / 127.0f;
        combined *= 0.78f;
        combined = combined / (1.0f + fabsf(combined));

        int32_t voiced = (int32_t)(combined * 32767.0f);
        if (voiced > 32767) voiced = 32767;
        if (voiced < -32768) voiced = -32768;
        note_voice_s = (int16_t)voiced;

        uint16_t note_env_step = note_voice_.env_step_q16;
        note_env_step = (uint16_t)(float(note_env_step) * (1.0f + 0.30f * mod_bus_.note_decay));
        if (note_voice_.env_q16 > note_env_step) {
            note_voice_.env_q16 = (uint16_t)(note_voice_.env_q16 - note_env_step);
        } else {
            note_voice_.env_q16 = 0u;
            note_voice_.active = false;
            note_voice_.op_env = 0.0f;
            note_voice_.op_mod_env = 0.0f;
        }

    }

    // ── Snapshot Voice Layer (v42) ───────────────────────────
    // Voz disparable por snapshot pad, independiente del paisaje base.
    int16_t snapshot_voice_s = 0;
    if (snapshot_voice_.active) {
        EvalContext sv_ctx = cached_ctx_;
        sv_ctx.note_mode_active = false;
        sv_ctx.note_pitch_ratio = 1.0f;
        sv_ctx.t = base_t + snapshot_voice_.t_offset;

        int16_t raw_sv = snapshot_voice_graph_.evaluate(sv_ctx);
        const float sv_env = (float)snapshot_voice_.env_q16 / 65535.0f;

        const float dt = 1.0f / 44100.0f;
        float root_freq = root_freq_cached_;
        if (root_freq < 40.0f) root_freq = 40.0f;
        if (root_freq > 880.0f) root_freq = 880.0f;
        snapshot_voice_.op_phase = wrap_phase01(snapshot_voice_.op_phase + root_freq * dt);
        snapshot_voice_.op_phase2 = wrap_phase01(snapshot_voice_.op_phase2 + (root_freq * 0.501f) * dt);
        snapshot_voice_.float_phase = f_wrap01(snapshot_voice_.float_phase + root_freq * dt);
        snapshot_voice_.float_phase2 = f_wrap01(snapshot_voice_.float_phase2 + (root_freq * 0.5f) * dt);

        const float osc = op_sine(snapshot_voice_.op_phase);
        const float tri = op_tri(snapshot_voice_.op_phase2);
        const float nz = op_noise(snapshot_voice_.op_noise_state);
        const float macro = cached_ctx_.macro;
        snapshot_voice_.op_mod_env *= 0.9975f;

        float synth = 0.0f;
        // operator_amount kept from earlier routing design; current blend is solved by bb/op/float/fb weights.
        float algo_trim = 1.0f;
        float algo_low_weight = 0.16f;
        switch (snapshot_voice_.op_algo) {
            default:
            case 0u: {
                const float mod = op_sine(snapshot_voice_.op_phase2) * (0.28f + snapshot_voice_.op_mod_env * 0.22f);
                const float pm = op_pm(snapshot_voice_.op_phase, mod, 1.1f + macro * 0.5f);
                synth = pm * 0.68f + tri * 0.20f + nz * 0.05f;
                synth = op_lp(synth, snapshot_voice_.op_lp_z, 0.08f);
                synth = op_slew(synth, snapshot_voice_.op_slew_z, 0.08f);
                synth = op_fold(synth, 0.18f + macro * 0.10f);
                synth = voice_shaper(synth, 0.28f, 0.18f + macro * 0.12f);
                algo_trim = 0.90f;
                algo_low_weight = 0.24f;
                break;
            }
            case 1u: {
                const float ring = op_ring(osc, nz);
                synth = osc * 0.34f + tri * 0.22f + ring * 0.30f + nz * 0.08f;
                synth = op_lp(synth, snapshot_voice_.op_lp_z, 0.12f);
                synth = op_slew(synth, snapshot_voice_.op_slew_z, 0.12f);
                synth = op_fold(synth, 0.30f + macro * 0.08f);
                synth = voice_shaper(synth, 0.32f, 0.28f + macro * 0.12f);
                algo_trim = 0.82f;
                algo_low_weight = 0.12f;
                break;
            }
            case 2u: {
                const float subosc = op_sine(wrap_phase01(snapshot_voice_.op_phase * 0.5f));
                const float pm = op_pm(snapshot_voice_.op_phase, tri * 0.10f, 0.45f);
                synth = subosc * 0.40f + pm * 0.26f + osc * 0.20f + nz * 0.04f;
                synth = op_lp(synth, snapshot_voice_.op_lp_z, 0.07f);
                synth = op_slew(synth, snapshot_voice_.op_slew_z, 0.10f);
                synth = voice_shaper(synth, 0.34f, 0.10f + macro * 0.08f);
                algo_trim = 0.88f;
                algo_low_weight = 0.78f;
                break;
            }
            case 3u: {
                const float pm = op_pm(snapshot_voice_.op_phase, nz * 0.18f, 0.9f);
                synth = pm * 0.30f + osc * 0.18f + nz * 0.26f + op_ring(tri, nz) * 0.12f;
                synth = op_lp(synth, snapshot_voice_.op_lp_z, 0.11f);
                synth = op_slew(synth, snapshot_voice_.op_slew_z, 0.10f);
                synth = op_fold(synth, 0.24f + macro * 0.12f);
                synth = voice_shaper(synth, 0.24f, 0.34f + macro * 0.16f);
                algo_trim = 0.76f;
                algo_low_weight = 0.14f;
                break;
            }
        }

        const float low_osc = op_sine(wrap_phase01(snapshot_voice_.op_phase * 0.5f));
        synth += low_osc * algo_low_weight * (0.08f + 0.32f * low_depth_macro_) * (0.20f + 0.60f * float_body_macro_);

        float float_voice = f_sine(snapshot_voice_.float_phase) * 0.64f
                          + f_sine(snapshot_voice_.float_phase2) * (0.20f + 0.20f * algo_low_weight);
        if (snapshot_voice_.op_algo == 1u || snapshot_voice_.op_algo == 3u) {
            float_voice += f_tri(snapshot_voice_.float_phase * 0.5f) * 0.08f;
        }
        float_voice = f_softclip(float_voice);
        float_voice *= sv_env * (0.10f + 0.28f * float_body_macro_) * (0.14f + 0.46f * algo_low_weight);

        float op_env = 0.0f;
        if (snapshot_voice_.latched && snapshot_voice_.env_step_q16 == 0u) {
            snapshot_voice_.op_env += (0.84f - snapshot_voice_.op_env) * 0.0025f;
            op_env = snapshot_voice_.op_env;
        } else {
            op_env = op_decay(snapshot_voice_.op_env, 0.0012f);
        }
        synth *= (op_env * algo_trim);
        const float drift = snapshot_drift_cached_ + op_sine(snapshot_voice_.op_phase2) * 0.0035f;
        synth *= (1.0f + drift);
        synth += snapshot_air_cached_ * sv_env;

        const float fb = floatbeat_algo(
            snapshot_voice_.fb_state,
            dt,
            root_freq,
            0.22f + 0.42f * algo_low_weight,
            snapshot_voice_.op_algo
        );

        const float fb_shaped = f_softclip(
            fb * (0.42f + 0.22f * algo_low_weight) +
            f_sine(snapshot_voice_.float_phase2) * (0.10f + 0.12f * float_body_macro_)
        ) * sv_env;

        float bb = (float)raw_sv / 32768.0f;

        float float_mix = 0.05f + 0.14f * float_body_macro_ + 0.03f * float_plane_bias_;
        float fb_mix    = 0.01f + 0.14f * floatbeat_mix_macro_ + 0.03f * float_plane_bias_;
        if (float_mix < 0.0f) float_mix = 0.0f;
        if (fb_mix < 0.0f) fb_mix = 0.0f;

        float bb_mix = 0.66f - 0.06f * float_plane_bias_
                     - 0.08f * float_body_macro_
                     - 0.08f * floatbeat_mix_macro_;

        if (bb_mix < 0.34f) bb_mix = 0.34f;

        float op_mix = 1.0f - bb_mix - float_mix - fb_mix;

        if (op_mix < 0.10f) {
            float deficit = 0.10f - op_mix;
            bb_mix -= deficit;
            if (bb_mix < 0.28f) bb_mix = 0.28f;
            op_mix = 1.0f - bb_mix - float_mix - fb_mix;
        }

        float combined =
            bb          * bb_mix +
            synth       * op_mix +
            float_voice * float_mix +
            fb_shaped   * fb_mix;
        combined *= sv_env;
        combined *= (float)snapshot_voice_.velocity / 127.0f;
        combined *= 0.72f;
        combined = combined / (1.0f + fabsf(combined));

        int32_t sv = (int32_t)(combined * 32767.0f);
        if (sv > 32767) sv = 32767;
        if (sv < -32768) sv = -32768;
        snapshot_voice_s = (int16_t)sv;

        if (snapshot_voice_.env_step_q16 > 0u) {
            if (snapshot_voice_.env_q16 > snapshot_voice_.env_step_q16) {
                snapshot_voice_.env_q16 = (uint16_t)(snapshot_voice_.env_q16 - snapshot_voice_.env_step_q16);
            } else {
                snapshot_voice_.env_q16 = 0u;
                snapshot_voice_.active = false;
                snapshot_voice_.latched = false;
                snapshot_voice_.op_env = 0.0f;
                snapshot_voice_.op_mod_env = 0.0f;
            }
        }

    }

    // ── Lead tonal ────────────────────────────────────────────
    // En Note Mode: el pitch viene directo del pad, no del byte alto del bytebeat
    if ((note_update_ctr_++ & 31u) == 0) {
        if (ctx.note_mode_active && pitch_ratio != 1.0f) {
            float freq_hz = pitch_ratio * 440.0f;
            if (freq_hz < 20.0f)   freq_hz = 20.0f;
            if (freq_hz > 8000.0f) freq_hz = 8000.0f;
            lead_osc_.set_freq_slew(freq_hz);
        } else {
            // FIX V1.21 B8: pitch_q movida al scope donde se asigna y usa.
            // Antes declarada fuera del else → warning de variable usada sin inicializar
            // si se tomaba el branch note_mode_active.
            uint8_t pitch_raw = (uint8_t)(((uint32_t)(synth_l + 32768) >> 9) & 0x7F);
            ScaleId sid = (ScaleId)ctx.scale_id;
            uint8_t pitch_q = Quantizer::quantize(pitch_raw, static_cast<uint8_t>(sid), ctx.root);

           uint8_t scale_id = (uint8_t)ctx.scale_id;
           uint8_t pitch_q = Quantizer::quantize(pitch_raw, scale_id, ctx.root);
            if (pitch_q != last_lead_note_) {
                lead_osc_.set_freq_slew(Quantizer::note_to_freq(pitch_q));
                last_lead_note_ = pitch_q;
            }
        }
    }

    // FM/AM: alimentar el bytebeat del frame actual al lead oscillator.
    // Se llama justo antes de process() para que la modulación use
    // el sample del ciclo actual (latencia cero perceptible).
    // Fuera de Note Mode feed_bytebeat igual corre pero fm_depth_=0 así que
    // no tiene costo de audio — solo actualiza el slew de bb_norm_.
    lead_osc_.feed_bytebeat(synth_l);

    int16_t lead_s = (int16_t)(((int32_t)lead_osc_.process() * 20000) >> 15);
    int16_t synth_mix_l, synth_mix_r;
    if (quant_mix_q15_ <= 0) {
        synth_mix_l = synth_l;
        synth_mix_r = synth_r;
    } else if (quant_mix_q15_ >= 32767) {
        synth_mix_l = lead_s;
        synth_mix_r = lead_s;
    } else {
        const int32_t qa   = quant_mix_q15_;
        const int32_t qa_c = 32767 - qa;
        synth_mix_l = (int16_t)((((int32_t)synth_l * qa_c) + ((int32_t)lead_s * qa)) >> 15);
        synth_mix_r = (int16_t)((((int32_t)synth_r * qa_c) + ((int32_t)lead_s * qa)) >> 15);
    }

    synth_mix_l = LevelScaler::scale(synth_mix_l, LevelScaler::SYNTH_GAIN);
    synth_mix_r = LevelScaler::scale(synth_mix_r, LevelScaler::SYNTH_GAIN);

    // ── Envelope AR (solo escena base) ───────────────────────
    if (!ctx.note_mode_active && env_gate_) {
        if (env_gate_hold_ctr_ > 0) {
            --env_gate_hold_ctr_;
        } else {
            env_gate_ = false;
        }
    }
    int32_t env_gain = envelope_.next_gain(env_gate_);
    synth_mix_l = ArEnvelope::apply(synth_mix_l, env_gain);
    synth_mix_r = ArEnvelope::apply(synth_mix_r, env_gain);

    // Macro Motion: paneo dinámico solo sobre la escena base.
    {
        float pan = macro_out_.pan;
        if (pan > 1.0f) pan = 1.0f;
        if (pan < -1.0f) pan = -1.0f;
        float pan_l = 1.0f - ((pan > 0.0f) ? pan : 0.0f);
        float pan_r = 1.0f + ((pan < 0.0f) ? pan : 0.0f);
        int32_t pl = (int32_t)(pan_l * 32767.0f);
        int32_t pr = (int32_t)(pan_r * 32767.0f);
        synth_mix_l = (int16_t)(((int32_t)synth_mix_l * pl) >> 15);
        synth_mix_r = (int16_t)(((int32_t)synth_mix_r * pr) >> 15);
    }

    // ── Drum engine + sidechain ──────────────────────────────
    int16_t drum_l = 0, drum_r = 0;
    int32_t sidechain_q15 = 32767;
    drums_.process(drum_l, drum_r, sidechain_q15);

    drum_l = LevelScaler::scale(drum_l, LevelScaler::DRUM_GAIN);
    drum_r = LevelScaler::scale(drum_r, LevelScaler::DRUM_GAIN);

    // ── Buses Stage 1B: scene / voice / drum + sub estable ───
    float scene_l = (float)synth_mix_l / 32768.0f;
    float scene_r = (float)synth_mix_r / 32768.0f;

    float voice_m = ((float)note_voice_s + (float)snapshot_voice_s) / 32768.0f;
    voice_m = process_voice_comb(voice_m, voice_comb_buf_, voice_comb_idx_, voice_comb_lp_);

    // Stage 3C: subgrave controlado por macro + cuerpo float en voices.
    const float target_sub = note_voice_.active
        ? note_voice_.base_freq_hz
        : root_freq_cached_;
    sub_freq_hz_ += (target_sub - sub_freq_hz_) * 0.020f;
    if (sub_freq_hz_ < 30.0f) sub_freq_hz_ = 30.0f;
    if (sub_freq_hz_ > 85.0f) sub_freq_hz_ = 85.0f;

    const float idle_floor = 0.04f + 0.32f * low_depth_macro_;
    const float active_peak = 0.42f + 0.56f * low_depth_macro_;
    const float breath_bass_scale = 0.88f + 0.28f * mod_bus_.bass_gate;
                                  + 0.16f * mod_bus_.bass_decay;
    const float sub_target_env = ((note_voice_.active || snapshot_voice_.active) ? active_peak : idle_floor)
                               * breath_bass_scale;
    sub_env_ += (sub_target_env - sub_env_) * ((note_voice_.active || snapshot_voice_.active) ? 0.012f : 0.0045f);

    sub_phase_ = wrap_phase01(sub_phase_ + sub_freq_hz_ * (1.0f / 44100.0f));
    float sub = fast_sine01(sub_phase_) * sub_env_;
    sub *= 0.74f;
    sub = sub / (1.0f + fabsf(sub));

    const float voice_abs = fabsf(voice_m);

    const float kick_env = drums_.kick_env();
    float sub_duck = 1.0f - (0.34f + 0.24f * low_depth_macro_) * kick_env;
    if (sub_duck < 0.30f) sub_duck = 0.30f;
    sub *= sub_duck;
    const float voice_duck = (voice_abs > 0.01f) ? 0.88f : 1.0f;
    const float kick_duck  = 1.0f - 0.28f * kick_env;
    const float voice_kick = 1.0f - 0.12f * kick_env;

    scene_l *= voice_duck * kick_duck * 0.85f;
    scene_r *= voice_duck * kick_duck * 0.85f;
    voice_m *= voice_kick * 0.95f;

    const float voice_pan_l = 0.96f;
    const float voice_pan_r = 0.90f;
    float voice_l = voice_m * voice_pan_l;
    float voice_r = voice_m * voice_pan_r;

    // Delay ducking: sólo el retorno wet se retrae con kick/voz.
    const float delay_duck_target = 1.0f - (0.30f * kick_env + 0.12f * clamp01(voice_abs * 1.6f));
    delay_duck_smooth_ += (delay_duck_target - delay_duck_smooth_) * 0.08f;
    dsp_.set_delay_wet_duck_fast(delay_duck_smooth_);

    // Procesar únicamente scene+voices por DSP.
    int32_t synth_fx_l_i32 = (int32_t)((scene_l + voice_l) * 32767.0f);
    if (synth_fx_l_i32 > 32767) synth_fx_l_i32 = 32767;
    if (synth_fx_l_i32 < -32768) synth_fx_l_i32 = -32768;
    int16_t synth_fx_l = (int16_t)synth_fx_l_i32;

    int32_t synth_fx_r_i32 = (int32_t)((scene_r + voice_r) * 32767.0f);
    if (synth_fx_r_i32 > 32767) synth_fx_r_i32 = 32767;
    if (synth_fx_r_i32 < -32768) synth_fx_r_i32 = -32768;
    int16_t synth_fx_r = (int16_t)synth_fx_r_i32;

    dsp_.process(synth_fx_l, synth_fx_r);

    int32_t out_l = 0;
    int32_t out_r = 0;
    if (output_mode_ == OUTPUT_SPLIT) {
        out_l = synth_fx_l;
        out_r = drum_r;
    } else {
        float sub_mix = (0.08f + 0.30f * low_depth_macro_ + 0.09f * perf_macro_ + 0.08f * float_body_macro_)
                      * (1.0f - 0.20f * macro_motion_.density_s)
                      * (0.90f + 0.16f * mod_bus_.bass_gate + 0.06f * breath_amount_ + (audio_decision_.kick_like ? 0.06f : 0.0f));
        if (sub_mix < 0.06f) sub_mix = 0.06f;
        if (sub_mix > 0.58f) sub_mix = 0.58f;

        float master_l = (float)synth_fx_l / 32768.0f + ((float)drum_l / 32768.0f) * 1.03f + sub * sub_mix;
        float master_r = (float)synth_fx_r / 32768.0f + ((float)drum_r / 32768.0f) * 1.03f + sub * sub_mix;
        master_l *= 0.78f;
        master_r *= 0.78f;
        master_l = master_l / (1.0f + fabsf(master_l));
        master_r = master_r / (1.0f + fabsf(master_r));
        out_l = (int32_t)(master_l * 32767.0f);
        out_r = (int32_t)(master_r * 32767.0f);
    }

    if (out_l >  32767) out_l =  32767;
    if (out_l < -32768) out_l = -32768;
    if (out_r >  32767) out_r =  32767;
    if (out_r < -32768) out_r = -32768;

    output_->write((int16_t)out_l, (int16_t)out_r);
}

// --- Stage2B integrated: operator routing (pm + ring + slew) ---
