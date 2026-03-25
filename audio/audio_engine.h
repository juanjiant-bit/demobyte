#pragma once
// audio_engine.h — Bytebeat Machine V1.21 / Build v42
// Engine de audio con separación control-rate/audio-rate, swing real y
// spread estéreo optimizado por segmentos.
#include "audio_output.h"
#include "drums/drum_engine.h"
#include "../synth/lead_osc.h"
#include "../synth/quantizer.h"
#include "../synth/note_mode.h"
#include "../dsp/dsp_chain.h"
#include "dsp/ar_envelope.h"
#include "operators.h"
#include "float_ops.h"
#include "floatbeat_seed.h"
#include "../utils/ring_buffer.h"
#include "../sequencer/event_types.h"
#include "../io/input_router.h"   // para PAD_MUTE, DrumParam
#include <cstdint>

class StateManager;

class AudioEngine {
public:
    enum OutputMode : uint8_t { OUTPUT_MASTER = 0, OUTPUT_SPLIT = 1 };
    static constexpr uint32_t SAMPLE_RATE = 44100;
    static constexpr uint16_t BLOCK_SIZE  = 32;

    void init(AudioOutput* output, StateManager* state);
    void run();
    void set_event_queue(RingBuffer<SequencerEvent, 128>* q);
    void set_output_mode(OutputMode mode) { output_mode_ = mode; }
    OutputMode output_mode() const { return output_mode_; }
    void update_breath_analysis(float mono_abs);

    void        process_one_sample();   // public: llamado directamente en tight loop

private:
    static bool timer_callback(repeating_timer_t* rt);
    void        generate_samples();
    void        drain_events();
    void        refresh_control_block();
    void        begin_spread_segment(const EvalContext& ctx, uint32_t base_t, float pitch_ratio);
    void        update_macro_motion(int32_t bb_i, float macro);
    void        finalize_macro_motion_block();
    void        update_modulation_bus();
    void        update_bass_movement();
    void        finalize_breath_analysis_block();
    void        update_audio_decision_state();
    void        update_dominant_decision();
    static inline float smoothstep(float edge0, float edge1, float x) {
        x = clamp01((x - edge0) / (edge1 - edge0));
        return x * x * (3.0f - 2.0f * x);
    }

    static inline int16_t lerp_i16(int16_t a, int16_t b, uint8_t frac_q8) {
        int32_t ai = a;
        int32_t bi = b;
        return (int16_t)(ai + (((bi - ai) * frac_q8) >> 8));
    }

    static inline float clamp01(float x) {
        return (x < 0.0f) ? 0.0f : ((x > 1.0f) ? 1.0f : x);
    }

    AudioOutput*                    output_      = nullptr;
    StateManager*                   state_mgr_   = nullptr;
    RingBuffer<SequencerEvent, 128>* event_queue_ = nullptr;
    DspChain                        dsp_;
    DrumEngine                      drums_;
    LeadOsc                         lead_osc_;
    uint8_t                         last_lead_note_  = 69;
    uint32_t                        note_update_ctr_ = 0;
    repeating_timer_t               timer_;


    struct NoteVoice {
        bool     active = false;
        uint8_t  degree = 0;
        uint8_t  velocity = 96;
        float    pitch_ratio = 1.0f;
        float    base_freq_hz = 220.0f;
        uint32_t t_offset = 0;
        uint16_t env_q16 = 0;
        uint16_t env_step_q16 = 1024;
        uint8_t  formula_source = 0; // 0=A, 1=B, 2=follow current morph
        float    op_phase = 0.0f;
        float    op_phase2 = 0.0f;
        float    op_lp_z = 0.0f;
        float    op_slew_z = 0.0f;
        float    float_phase = 0.0f;
        float    float_phase2 = 0.0f;
        FloatbeatState fb_state = {};
        float    op_env = 0.0f;
        float    op_mod_env = 0.0f;
        uint8_t  op_algo = 0u;
        uint32_t op_noise_state = 0x13579BDFu;
        float    pitch_env = 0.0f;
    };

    struct SnapshotVoice {
        bool     active = false;
        bool     latched = false;
        uint8_t  slot = 0xFFu;
        uint8_t  velocity = 110u;
        uint16_t env_q16 = 0;
        uint16_t env_step_q16 = 1024u;
        uint32_t t_offset = 0u;
        float    op_phase = 0.0f;
        float    op_phase2 = 0.0f;
        float    op_lp_z = 0.0f;
        float    op_slew_z = 0.0f;
        float    float_phase = 0.0f;
        float    float_phase2 = 0.0f;
        FloatbeatState fb_state = {};
        float    op_env = 0.0f;
        float    op_mod_env = 0.0f;
        uint8_t  op_algo = 0u;
        uint32_t op_noise_state = 0x2468ACE1u;
    };

    struct MacroMotionState {
        float    rhythm_raw  = 0.0f;
        float    chaos_raw   = 0.0f;
        float    density_raw = 0.0f;
        float    rhythm_s    = 0.0f;
        float    chaos_s     = 0.0f;
        float    density_s   = 0.0f;
        float    pan_s       = 0.0f;
        float    macro_s     = 0.0f;
        float    macro_last  = 0.0f;
        float    rhythm_accum = 0.0f;
        float    transient_accum = 0.0f;
        float    density_accum = 0.0f;
        float    chaos_accum = 0.0f;
        uint8_t  accum_count = 0u;
        uint8_t  chaos_count = 0u;
        uint8_t  chaos_decim = 0u;
        int32_t  prev_bb     = 0;
        // LFSR Galois 16-bit para chaos real (independiente del contenido del bytebeat)
        // Polinomio x^16+x^15+x^13+x^4+1 — período 65535
        uint16_t lfsr_state  = 0xACE1u;
    };

    struct MacroMotionOutputs {
        float pan = 0.0f;
        float drive_mod = 0.0f;
        float grain_mod = 0.0f;
        float chorus_mod = 0.0f;
        float reverb_mod = 0.0f;
    };



    struct BreathAnalysisState {
        float env = 0.0f;
        float spike = 0.0f;
        float low_proxy = 0.0f;
        float high_proxy = 0.0f;

        float acc_abs = 0.0f;
        float acc_diff = 0.0f;
        float prev_abs = 0.0f;
        uint32_t count = 0;
    };

    struct AudioDecisionState {
        bool kick_like = false;
        bool snare_like = false;
        bool energy_peak = false;
        bool energy_drop = false;

        float prev_env = 0.0f;
        uint32_t peak_hold = 0;
        uint32_t drop_hold = 0;
    };

    struct ModulationBus {
        float breath_amount = 0.0f;
        float harmonic_blend = 0.5f;

        float note_fm_byte = 0.0f;
        float note_fm_float = 0.0f;
        float note_fold = 0.0f;
        float note_decay = 0.0f;

        float bass_gate = 0.0f;
        float bass_decay = 0.0f;

        float drum_decay = 0.0f;
        float stutter_amount = 0.0f;
        float harmonic_support = 1.0f;
    };

    MacroMotionState macro_motion_ = {};
    MacroMotionOutputs macro_out_ = {};
    ModulationBus mod_bus_ = {};
    BreathAnalysisState breath_analysis_ = {};
    AudioDecisionState audio_decision_ = {};

    enum class DominantDecision {
        NONE = 0,
        PEAK,
        KICK,
        SNARE,
        DROP
    };

    DominantDecision dominant_decision_ = DominantDecision::NONE;
    uint8_t decision_hold_counter_ = 0;
    float    breath_env_mix_ = 0.0f;
    float    breath_spike_mix_ = 0.0f;
    float    note_fm_mix_ = 0.0f;
    float    note_harmonic_mix_ = 0.0f;
    float    note_dynamic_boost_ = 0.0f;
    float    note_clarity_mix_ = 0.0f;
    float    note_layer_trim_ = 1.0f;
    float    fx_gate_boost_ = 0.0f;
    float    fx_stutter_boost_ = 0.0f;

    float    current_bpm_      = 120.0f;
    float    root_freq_cached_   = 55.0f;
    float    global_time_       = 0.0f;
    float    slow_phase_a_      = 0.0f;
    float    slow_phase_b_      = 0.0f;
    float    snapshot_drift_cached_ = 0.0f;
    float    snapshot_air_cached_   = 0.0f;
    float    sub_phase_         = 0.0f;
    float    sub_env_           = 0.0f;
    float    sub_freq_hz_       = 55.0f;
    float    delay_duck_smooth_ = 1.0f;
    float    low_depth_macro_    = 0.45f;
    float    float_body_macro_   = 0.35f;
    float    floatbeat_mix_macro_ = 0.18f;
    float    layer_morph_macro_   = 0.35f;
    float    low_depth_ctrl_      = 0.45f;
    float    float_body_ctrl_     = 0.35f;
    float    morph_ctrl_          = 0.35f;
    float    perf_macro_          = 0.0f;
    float    floatbeat_dry_macro_ = 0.0f;
    float    float_plane_bias_    = 0.0f;
    // High-level macro semantics derived from the legacy parameter map:
    // P5 / FILTER_MACRO -> breath_amount_  (amount of autonomous movement)
    // P6 / MORPH        -> harmonic_blend_ (dry harmonic -> hybrid -> destructive)
    float    breath_amount_      = 0.0f;
    float    harmonic_blend_     = 0.5f;
    int      bass_note_offset_   = 0;
    uint8_t  bass_seq_step_      = 0;
    uint8_t  bass_pattern_id_    = 0;
    uint32_t bass_seq_accum_     = 0;
    uint8_t  bass_last_seq_step_ = 0xFF;
    bool     bass_transport_lock_ = true;
    float algo_morph_bias_[4] = {0.0f, 0.0f, 0.1f, -0.05f};
    float    voice_comb_buf_[256] = {};
    uint16_t voice_comb_idx_ = 0;
    float    voice_comb_lp_ = 0.0f;
    NoteVoice note_voice_ = {};
    SnapshotVoice snapshot_voice_ = {};
    BytebeatGraph note_voice_graph_{};
    BytebeatGraph snapshot_voice_graph_{};
    volatile OutputMode output_mode_ = OUTPUT_MASTER;
    float    stutter_depth_    = 0.0f;
    float    note_pitch_ratio_ = 1.0f;
    EvalContext cached_ctx_    = {};
    AudioSnapshot control_snapshot_ = {};
    uint32_t    vibrato_phase_q24_ = 0;
    static constexpr uint32_t VIBRATO_RATE_Q24 = 2000; // ~5.3 Hz @ 44.1kHz

    // Spread runtime: evalúa el canal R a menor tasa y lo interpola por tramos
    // para evitar una segunda evaluación completa del graph en cada sample.
    uint8_t  spread_stride_      = 4;
    uint8_t  spread_phase_       = 0;
    uint8_t  spread_phase_inc_q8_= 64;
    uint16_t spread_mix_q15_     = 0;
    uint32_t spread_t_offset_    = 1;
    float    spread_macro_delta_ = 0.0f;
    int16_t  spread_seg_start_   = 0;
    int16_t  spread_seg_end_     = 0;

    bool     fx_freeze_on_       = false;
    bool     fx_oct_down_on_     = false;
    bool     fx_oct_up_on_       = false;
    bool     fx_vibrato_on_      = false;
    bool     fx_beat_repeat_on_  = false;  // V1.18: beat repeat (pot div)
    bool     fx_repeat16_on_     = false;  // forced 1/16
    bool     fx_repeat8_on_      = false;  // forced 1/8
    bool     fx_repeat4_on_      = false;  // forced 1/4
    float    fx_pitch_mul_current_ = 1.0f;
    float    vibrato_depth_current_ = 0.0f;

    // V1.10 — Envelope AR
    ArEnvelope  envelope_;
    bool        env_gate_     = false;  // gate actual (pad presionado o seq activa)
    static constexpr uint32_t ENV_GATE_HOLD = 88; // ~2ms @ 44100Hz (V1.11)
    uint32_t    env_gate_hold_ctr_ = 0;

    uint32_t accumulator_  = 0;
    static constexpr uint32_t ACCUM_ADD = 441;
    static constexpr uint32_t ACCUM_TOP = 10;    // FIX: 441/10=44.1 samples/ms=44100/sec


    // Cache de control-rate para evitar recalcular setters del DSP sin cambios
    float base_drive_         = 0.0f;
    float base_reverb_room_   = 0.0f;
    float base_reverb_wet_    = 0.0f;
    float base_chorus_        = 0.0f;
    float base_grain_         = 0.0f;
    float cached_drive_       = -1.0f;
    float cached_reverb_room_ = -1.0f;
    float cached_reverb_wet_  = -1.0f;
    float cached_chorus_      = -1.0f;
    float cached_hp_          = -1.0f;
    float cached_grain_       = -1.0f;
    float cached_snap_        = -1.0f;
    float cached_beat_repeat_  = -1.0f;  // reservado
    float cached_attack_      = -1.0f;
    float cached_release_     = -1.0f;
    float cached_env_loop_t_  = -1.0f;
    float cached_drum_color_  = -1.0f;
    float cached_drum_decay_  = -1.0f;
    float cached_duck_        = -1.0f;

    // Aftertouch: modulaciones momentáneas superpuestas al valor base.
    // Se suman al base_ correspondiente en refresh_control_block().
    // Se resetean a 0.0 cuando la presión baja del DEADZONE del input_router.
    float at_reverb_wet_      = 0.0f;  // SNAP pads → reverb wet adicional
    float at_grain_wet_       = 0.0f;  // MUTE pad  → grain freeze wet adicional
    float cached_bpm_for_snap_= -1.0f;
    // V1.17: Delay
    float cached_delay_div_   = -1.0f;
    float cached_delay_fb_    = -1.0f;
    float cached_delay_wet_   = -1.0f;
    float cached_bpm_for_delay_= -1.0f;

    int32_t quant_mix_q15_    = 0;

    volatile uint32_t sample_tick_ = 0;
};
