#include <stdint.h>
#pragma once
// state_manager.h — Bytebeat Machine V1.21
// Estado central del firmware: snapshots, parámetros live, encoder y
// ruteo coherente entre patch, bus FX y controles globales.
#include <cstdint>
#include "snapshot_event.h"
#include "encoder_state.h"
#include "../synth/bytebeat_graph.h"
#include "../synth/quantizer.h"
#include "../synth/glide.h"
#include "hardware/sync.h"
#include "../sequencer/event_types.h"

static constexpr uint8_t SNAPSHOT_ENGINE_VERSION = 2;

struct Snapshot {
    uint8_t  snapshot_version;
    uint32_t seed;
    uint8_t  zone;
    float    macro;
    float    glide_time;
    float    time_div;
    float    tonal;
    float    spread;
    float    filter_cutoff;
    float    fx_amount;
    float    drive;
    float    reverb_room;   // 0.0-1.0, default 0.84
    float    reverb_wet;    // 0.0-1.0, default 0.25
    // V1.18 — Delay (div+wet en snapshot; fb queda como bus global)
    float    delay_div;     // 0.0-1.0 → índice 0..10, default 0.40 (1/4)
    float    delay_wet;     // 0.0-1.0, default 0.0 (delay off)
    // V1.6
    uint8_t  scale_id;      // ScaleId 0-7, default MAJOR(1)
    uint8_t  root;          // 0-11, default C(0)
    float    drum_color;    // 0.0-1.0 (family selector), default 0.0
    float    drum_decay;    // 0.0-1.0 (decay scale), default 0.5
    // V1.10 — Envelope
    float    env_release;   // 0.0-1.0 → 1ms..8s, default 0.0 (snappy)
    float    env_attack;    // 0.0-1.0 → 1ms..600ms, default 0.0
    bool     env_loop;      // default false

    // BYT3 parametric bytebeat snapshot payload
    uint8_t  formula_a;
    uint8_t  formula_b;
    uint8_t  morph;
    uint8_t  rate;
    uint8_t  shift;
    uint8_t  mask;
    uint8_t  feedback;
    uint8_t  jitter;
    uint8_t  phase;
    uint8_t  xor_fold;
    uint8_t  bb_seed;
    uint8_t  filter_macro;
    uint8_t  resonance;
    uint8_t  env_macro;

    bool     valid;
};

static constexpr float   TIME_DIV_STEPS[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
static constexpr uint8_t TIME_DIV_COUNT   = 5;

inline float quantize_time_div(float pot_val) {
    uint8_t idx = (uint8_t)(pot_val * (TIME_DIV_COUNT - 1) + 0.5f);
    if (idx >= TIME_DIV_COUNT) idx = TIME_DIV_COUNT - 1;
    return TIME_DIV_STEPS[idx];
}

enum class RandomizeMode : uint8_t { CONTROLLED = 0, WILD = 1 };

struct AudioSnapshot {
    EvalContext ctx = {};
    float reverb_room = 0.84f;
    float reverb_wet = 0.25f;
    float drive = 0.0f;
    float glide = 0.1f;
    uint8_t scale_id = 1u;
    uint8_t root = 0u;
    float drum_color = 0.0f;
    float drum_decay = 0.5f;
    float duck_amount = 0.0f;
    float chorus = 0.0f;
    float hp = 0.0f;
    float grain = 0.0f;
    float snap = 0.0f;
    float delay_div = 0.40f;
    float delay_fb = 0.40f;
    float delay_wet = 0.00f;
    float beat_repeat_div = 0.5f;
    float env_release = 0.0f;
    float env_attack = 0.0f;
    bool  env_loop = false;
    float env_loop_time = 1.0f;
    float bpm = 120.0f;
    bool note_mode_active = false;
    float note_pitch_ratio = 1.0f;
    bool note_active = false;
    uint8_t note_degree = 0xFFu;
    uint8_t note_midi = 0xFFu;
    uint8_t note_voice_source = 0u;
    uint8_t current_step_index = 0xFFu;
    bool transport_running = false;
};

class StateManager {
public:
    static constexpr uint8_t NUM_SNAPSHOTS = 8;
    static constexpr uint8_t NO_PENDING    = 0xFF;

    void init();

    // ── Core0 only ───────────────────────────────────────────
    void    process_pending();
    void    fill_context(EvalContext& ctx) const;  // no toca ctx.t
    int16_t evaluate(const EvalContext& ctx);
    int16_t evaluate_preview(const EvalContext& ctx) const;

    uint8_t get_active_slot()  const { return active_slot_; }

    // ── Core1 ────────────────────────────────────────────────
    void request_trigger(uint8_t slot);
    void request_save   (uint8_t slot, const float pots[7]);
    void set_patch_param(ParamId id, float value);
    void set_bus_param  (ParamId id, float value);
    void set_aftertouch_macro(float pressure);
    float get_param_normalized(ParamId id) const;
    uint32_t get_snapshot_epoch() const { return snapshot_epoch_; }
    AudioSnapshot make_audio_snapshot() const;
    bool  start_snapshot_morph(uint8_t slot_a, uint8_t slot_b);
    void  update_snapshot_morph(float amount);
    void  stop_snapshot_morph(bool commit_to_target);
    bool  is_snapshot_morph_active() const { return snapshot_morph_active_; }
    uint8_t get_snapshot_morph_a() const { return snapshot_morph_a_; }
    uint8_t get_snapshot_morph_b() const { return snapshot_morph_b_; }
    float get_snapshot_morph_amount() const { return snapshot_morph_amount_; }
    void randomize_all(RandomizeMode mode);
    void mutate_active_snapshot(float amount, bool wild = false);

    // Quantizer / root / scale (Core1)
    void set_scale(uint8_t sid);
    void set_root(uint8_t r);
    void set_zone_live(uint8_t zone);
    void set_seed_variation_live(float v);
    uint8_t get_zone_live() const { return ctx_.zone; }
    float get_seed_variation_live() const { return seed_variation_live_; }

    // Encoder contextual (preparación V1.14)
    void next_encoder_mode();

    // HOME: resetea el estado de performance sin tocar snapshots ni secuencia.
    // Nivel 1 (soft): encoder → BPM, bus params (chorus, hp, reverb) → defaults
    //                 del snapshot activo, pot virtual tracking recalibrado.
    // Nivel 2 (full): ídem nivel 1 + drum mutes → false + punch FX activos off.
    enum class HomeLevel : uint8_t { SOFT = 0, FULL = 1 };
    void home_reset(HomeLevel level);
    void encoder_delta(int delta, bool shifted = false);
    EncoderMode get_encoder_mode() const { return encoder_.mode; }
    const EncoderState& get_encoder_state() const { return encoder_; }
    float get_mutate_amount() const { return encoder_.mutate_amount; }
    float get_swing_amount()  const { return encoder_.swing_amount;  }
    float get_density_amount() const { return encoder_.density_amount; }
    float get_chaos_amount() const { return encoder_.chaos_amount; }
    float get_space_amount() const { return encoder_.space_amount; }
    void set_space_macro(float v);

    // Drum/bus live controls (Core1 escribe, Core0 lee en safe points).
    void set_drum_color_live(float v) {
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        begin_live_write();
        drum_color_live_ = v;
        end_live_write();
    }
    void set_drum_decay_live(float v) {
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        begin_live_write();
        drum_decay_live_ = v;
        end_live_write();
    }
    void set_duck_amount_live(float v) {
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        begin_live_write();
        duck_amount_live_ = v;
        end_live_write();
    }
    // Bus FX live setters (layout final de 3 capas).
    void set_reverb_wet_live(float v) {
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        begin_live_write();
        reverb_wet_live_ = v;
        end_live_write();
    }
    void set_chorus_live  (float v) { begin_live_write(); chorus_live_   = clamp01(v); end_live_write(); }
    void set_hp_live      (float v) { begin_live_write(); hp_live_       = clamp01(v); end_live_write(); }
    void set_grain_live   (float v) { begin_live_write(); grain_live_    = clamp01(v); end_live_write(); }
    void set_snap_live    (float v) { begin_live_write(); snap_live_     = clamp01(v); end_live_write(); }
    // V1.17: Delay tempo-sync
    void set_delay_div(float v)  { begin_live_write(); delay_div_live_ = clamp01(v); end_live_write(); }
    void set_delay_fb (float v)  { begin_live_write(); delay_fb_live_  = clamp01(v); end_live_write(); }
    void set_delay_wet(float v)  { begin_live_write(); delay_wet_live_ = clamp01(v); end_live_write(); }
    void set_beat_repeat_div_live(float v) { begin_live_write(); beat_repeat_div_live_ = clamp01(v); end_live_write(); }
    // Patch/envelope live setters.
    void set_env_release (float v) { begin_live_write(); env_release_live_ = clamp01(v); end_live_write(); }
    void set_env_attack  (float v) { begin_live_write(); env_attack_live_  = clamp01(v); end_live_write(); }
    void set_env_loop    (bool  b) { begin_live_write(); env_loop_live_    = b; end_live_write(); }
    void set_env_loop_time(float v){ begin_live_write(); env_loop_time_live_ = clamp01(v); end_live_write(); }

    float get_chorus_live()   const { return chorus_live_;  }
    float get_hp_live()       const { return hp_live_;      }
    float get_grain_live()    const { return grain_live_;   }
    float get_snap_live()     const { return snap_live_;    }
    // V1.17: Delay
    float get_delay_div()  const { return delay_div_live_; }
    float get_delay_fb()   const { return delay_fb_live_;  }
    float get_delay_wet()  const { return delay_wet_live_; }
    float get_beat_repeat_div_live() const { return beat_repeat_div_live_; }
    float get_env_release ()  const { return env_release_live_;   }
    float get_env_attack  ()  const { return env_attack_live_;    }
    bool  get_env_loop    ()  const { return env_loop_live_;      }
    float get_env_loop_time() const { return env_loop_time_live_; }

    // Note Mode (Core1)
    void set_note_mode(bool active) { begin_live_write(); note_mode_active_ = active; end_live_write(); }
    bool is_note_mode()       const { return note_mode_active_; }

    // Prepara un graph paralelo para la Note Voice Layer sin afectar el motor base.
    // formula_source: 0 = Formula A, 1 = Formula B, 2 = Morph actual.
    void prepare_note_voice_graph(BytebeatGraph& dst, uint8_t formula_source = 0xFFu) const;
    void prepare_snapshot_voice_graph(BytebeatGraph& dst, uint8_t slot) const;

    enum : uint8_t {
        NOTE_VOICE_SOURCE_A     = 0u,
        NOTE_VOICE_SOURCE_B     = 1u,
        NOTE_VOICE_SOURCE_MORPH = 2u
    };

    void set_note_voice_source(uint8_t v) { begin_live_write(); note_voice_source_ = (v > 2u) ? 0u : v; end_live_write(); }
    uint8_t note_voice_source() const { return note_voice_source_; }
    void cycle_note_voice_source() { begin_live_write(); note_voice_source_ = (uint8_t)((note_voice_source_ + 1u) % 3u); end_live_write(); }
    const char* note_voice_source_name() const {
        switch (note_voice_source_) {
            case NOTE_VOICE_SOURCE_A: return "NOTE A";
            case NOTE_VOICE_SOURCE_B: return "NOTE B";
            case NOTE_VOICE_SOURCE_MORPH: return "NOTE MORPH";
            default: return "NOTE";
        }
    }

    // Nota activa del Note Mode (Core1 escribe, Core0 lee en fill_context/UI)
    // note_pitch_ratio = freq_nota / freq_base_A4
    void set_note_pitch(float ratio) { begin_live_write(); note_pitch_ratio_ = ratio; end_live_write(); }
    void clear_note_pitch()          { begin_live_write(); note_pitch_ratio_ = 1.0f; end_live_write(); }

    void set_active_note(uint8_t degree, uint8_t midi) {
        begin_live_write();
        note_active_ = true;
        note_degree_ = degree;
        note_midi_   = midi;
        end_live_write();
    }
    void clear_active_note() {
        begin_live_write();
        note_active_ = false;
        note_degree_ = 0xFFu;
        note_midi_   = 0xFFu;
        end_live_write();
    }
    bool    note_active() const { return note_active_; }
    uint8_t note_degree() const { return note_degree_; }
    uint8_t note_midi()   const { return note_midi_; }

    // Flash persistence (Core1, con Core0 pausado)
    bool flash_save();
    bool flash_load();

    // V1.14: mute por canal — Core1 escribe, Core0 lee en drain_events
    void toggle_mute_drum(uint8_t i) {
        if (i == 0) mute_kick_  = !mute_kick_;
        else if (i == 1) mute_snare_ = !mute_snare_;
        else if (i == 2) mute_hat_   = !mute_hat_;
    }
    bool get_mute_drum(uint8_t i) const {
        if (i == 0) return mute_kick_;
        if (i == 1) return mute_snare_;
        if (i == 2) return mute_hat_;
        return false;
    }
    void set_mute_snap (uint8_t slot, bool v) {
        if (slot < NUM_SNAPSHOTS) mute_snap_[slot] = v;
    }
    bool get_mute_kick ()              const { return mute_kick_;       }
    bool get_mute_snare()              const { return mute_snare_;      }
    bool get_mute_hat  ()              const { return mute_hat_;        }
    bool get_mute_snap (uint8_t slot)  const {
        return (slot < NUM_SNAPSHOTS) ? mute_snap_[slot] : false;
    }


    // Getters (Core0 lee en safe point)
    float   get_drive_live()      const { return drive_live_;        }
    float   get_glide_live()      const { return glide_live_;        }
    float   get_bpm()             const { return encoder_.bpm;       }
    float   get_reverb_room()     const { return reverb_room_live_;  }
    float   get_reverb_wet()      const { return reverb_wet_live_;   }
    uint8_t get_scale_id()        const { return scale_id_live_;     }
    uint8_t get_root()            const { return root_live_;         }
    float   get_drum_color_live()  const { return drum_color_live_;   }
    float   get_drum_decay_live()  const { return drum_decay_live_;   }
    float   get_duck_amount_live() const { return duck_amount_live_;  }

    // Acceso a snapshots para FlashStore
    const Snapshot* get_snapshots() const { return snapshots_; }
    Snapshot*       get_snapshots()       { return snapshots_; }

    // Transport/step exposure used by audio-side consumers such as the bass engine.
    uint8_t get_current_step_index() const;
    bool is_transport_running() const;
    void set_transport_running(bool running);
    void set_current_step_index(uint8_t step);

private:
    volatile uint8_t current_step_index_ = 0xFFu;
    volatile bool transport_running_ = false;
    void     do_trigger(uint8_t slot);
    void     do_save   (uint8_t slot, const float pots[7]);
    void     sync_live_from_snapshot(const Snapshot& s, bool rebuild_graph);
    static void apply_snapshot_engine_to_graph(BytebeatGraph& graph, const Snapshot& s);
    uint32_t generate_seed(uint8_t slot);
    uint32_t rng_next();
    float    rand01();
    float    rand_range(float lo, float hi);
    uint8_t  rand_u8(uint8_t max_exclusive);
    void     apply_space_macro();
    void     set_graph_param_on_live_graphs(ParamId id, uint8_t value);
    bool     apply_graph_param(ParamId id, float value);
    void     load_factory_bank();
    void     sanitize_snapshot(Snapshot& s, uint8_t slot_hint);
    uint32_t derive_seed_from_variation(float v) const;
    void     rebuild_active_graph(uint32_t seed, uint8_t zone);
    void     apply_snapshot_morph_preview(float amount);
    
    BytebeatGraph graphs_[2];
    uint8_t       active_graph_   = 0;
    uint8_t       incoming_graph_ = 1;
    Glide         glide_;
    uint8_t       pending_slot_   = NO_PENDING;
    uint8_t       active_slot_    = 0;
    Snapshot      snapshots_[NUM_SNAPSHOTS] = {};
    EvalContext   ctx_ = {};

    // Live fields — escritos por Core1, leídos por Core0 en safe points
    float         reverb_room_live_ = 0.84f;
    float         reverb_wet_live_  = 0.25f;
    float         drive_live_       = 0.0f;
    float         glide_live_       = 0.1f;
    uint8_t       scale_id_live_    = 1;    // MAJOR
    uint8_t       root_live_        = 0;    // C
    float         drum_color_live_  = 0.0f;
    float         drum_decay_live_  = 0.5f;
    float         duck_amount_live_ = 0.0f;

    // V1.7 — Note Mode live fields
    volatile bool  note_mode_active_ = false;  // Core1 escribe, Core0 lee
    volatile float note_pitch_ratio_ = 1.0f;    // ratio de la nota activa
    volatile bool  note_active_      = false;   // solo UI/estado musical
    volatile uint8_t note_degree_    = 0xFFu;
    volatile uint8_t note_midi_      = 0xFFu;
    volatile uint8_t note_voice_source_ = NOTE_VOICE_SOURCE_A;

    // Bus FX live fields (no pertenecen al snapshot).
    float          chorus_live_  = 0.0f;
    float          hp_live_      = 0.0f;
    float          grain_live_   = 0.0f;
    float          snap_live_    = 0.0f;
    // V1.17: Delay
    // Delay: div+wet se persisten en snapshot. fb es bus global performático.
    // home_reset(SOFT) restaura div+wet desde snapshot activo; fb siempre a 40%.
    float          delay_div_live_ = 0.40f;   // default idx≈4 (1/4 negra)
    float          delay_fb_live_  = 0.40f;
    float          delay_wet_live_ = 0.00f;
    float          beat_repeat_div_live_ = 0.5f;
    float          seed_variation_live_ = 0.0f;
    // V1.10 — Envelope live fields (Core1 escribe, Core0 lee)
    volatile float env_release_live_   = 0.0f;
    volatile float env_attack_live_    = 0.0f;
    volatile bool  env_loop_live_      = false;
    volatile float env_loop_time_live_ = 1.0f;
    volatile uint32_t live_state_seq_  = 0u;
    EncoderState   encoder_            = {};
    uint32_t       random_state_       = 0x13579BDFu;
    uint32_t       snapshot_epoch_      = 0;
    bool           snapshot_morph_active_ = false;
    uint8_t        snapshot_morph_a_      = 0xFFu;
    uint8_t        snapshot_morph_b_      = 0xFFu;
    float          snapshot_morph_amount_ = 0.0f;
    float          snapshot_morph_target_ = 0.0f;

    static inline void compiler_barrier() {
#if defined(__GNUC__)
        __asm__ volatile("" ::: "memory");
#endif
    }

    void begin_live_write() {
        ++live_state_seq_;
        compiler_barrier();
    }

    void end_live_write() {
        compiler_barrier();
        ++live_state_seq_;
    }

    static float clamp01(float v) {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    SnapshotEvent pending_event_;
    bool          event_ready_  = false;
    spin_lock_t*  lock_         = nullptr;

    // Mute state — Core1 escribe via setters, Core0 lee en drain_events
    bool          mute_kick_         = false;
    bool          mute_snare_        = false;
    bool          mute_hat_          = false;
    bool          mute_snap_[NUM_SNAPSHOTS] = {};
};
