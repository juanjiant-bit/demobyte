#pragma once
// sequencer.h — transport clock, swing and generative sequencing
#include <cstdint>
#include "hardware/sync.h"
#include "../utils/ring_buffer.h"
#include "event_types.h"
#include "../state/state_manager.h"
#include "clock_out.h"

enum class ClockSource { INT, EXT };
enum class PlayState   { STOPPED, ARMED, PLAYING, RECORDING };

enum class StepCondition : uint8_t {
    ALWAYS = 0,
    EVERY_2,
    EVERY_4,
    RANDOM_50,
    COUNT
};

struct SequenceStep {
    uint8_t  snapshot_mask = 0;
    uint8_t  note_mask     = 0;
    uint8_t  note_tie_mask = 0;
    uint8_t  drum_mask     = 0;
    uint8_t  snapshot_voice_mask = 0;
    uint8_t  snapshot_voice_latch_mask = 0;
    uint8_t  fx_hold_mask  = 0;
    bool     arp_enabled   = false;
    bool     fill_enabled  = false;
    uint8_t  condition     = static_cast<uint8_t>(StepCondition::ALWAYS);
    uint64_t param_mask    = 0;
    uint8_t  param_values[PARAM_LOCK_LAST + 1] = {};
    uint8_t  note_midi[8]  = {};
    uint8_t  snapshot_voice_vel_q7[8] = {};
    uint8_t  snapshot_voice_gate_steps[8] = {};
    uint8_t  chance_q8     = 255;
    uint8_t  ratchet_count = 1;
    int8_t   micro_timing_ticks = 0;
};

enum class PlayDirection : uint8_t {
    FORWARD = 0,
    REVERSE,
    PENDULUM,
    RANDOM,
    COUNT
};

enum class GrooveTemplate : uint8_t {
    STRAIGHT = 0,
    MPC,
    SHUFFLE,
    BROKEN,
    TRIPLET,
    IDM,
    COUNT
};

enum class IDMVariant : uint8_t {
    TIGHT = 0,
    BROKEN,
    SKITTER,
    MELT,
    COUNT
};

class Sequencer {
public:
    static constexpr uint8_t  INT_PPQN  = 96;
    static constexpr uint8_t  EXT_PPQN  = 24;
    static constexpr uint8_t  INT_MULT  = INT_PPQN / EXT_PPQN;  // 4
    static constexpr uint16_t MAX_TICKS = INT_PPQN * 2;

    void init();
    void set_clock_source(ClockSource src);
    void set_bpm(float bpm);
    void set_swing(float swing);
    void set_groove_template(GrooveTemplate tpl);
    void adjust_groove_template(int delta);
    GrooveTemplate groove_template() const { return groove_template_; }
    void set_play_direction(PlayDirection dir);
    void adjust_play_direction(int delta);
    PlayDirection play_direction() const { return play_direction_; }
    void set_idm_variant(IDMVariant variant);
    void adjust_idm_variant(int delta);
    IDMVariant idm_variant() const { return idm_variant_; }
    void set_density(float amount);
    void set_chaos(float amount);
    float density() const { return density_; }
    float chaos() const { return chaos_; }

    void update_int(uint64_t now_us);
    void on_ext_tick();

    void play();
    void stop();
    void rec_toggle();
    void rearm();
    void arm_record();

    bool     is_step_write_mode() const { return manual_step_write_; }
    bool     is_armed_record() const { return armed_record_; }
    bool     has_sequence() const { return sequence_valid_ && seq_length_ > 0; }
    uint8_t  sequence_length() const { return seq_length_; }
    uint8_t  current_step_index() const { return play_step_; }
    uint8_t  current_write_step_index() const { return write_step_; }
    uint8_t  last_emitted_step_index() const { return last_step_ == 0xFF ? 0 : last_step_; }
    bool     is_overdub() const { return play_state_ == PlayState::RECORDING && !manual_step_write_; }
    bool     step_has_snapshot(uint8_t step) const;
    bool     step_has_note(uint8_t step) const;
    bool     step_has_drum(uint8_t step) const;
    bool     step_has_snapshot_voice(uint8_t step) const;
    bool     step_has_motion(uint8_t step) const;
    bool     step_has_any(uint8_t step) const;
    uint8_t  visible_page_base() const;
    uint8_t  current_edit_step_index() const;
    uint8_t  preroll_steps_left() const { return preroll_steps_left_; }

    void on_manual_advance();
    void clear_current_snapshot_layer();
    void clear_current_note(uint8_t logical_pad);
    void toggle_current_note_tie(uint8_t logical_pad);
    void clear_current_drum_layer();
    void clear_current_param_lock(ParamId id);
    void clear_current_step();
    void duplicate_previous_step_into_next();
    void copy_current_page();
    void paste_page_to_current_page();
    float current_step_chance() const;
    void adjust_current_step_chance(int delta);
    void reset_current_step_chance();
    uint8_t current_step_ratchet() const;
    void adjust_current_step_ratchet(int delta);
    void reset_current_step_ratchet();
    int8_t current_step_microtiming() const;
    void adjust_current_step_microtiming(int delta);
    void reset_current_step_microtiming();
    StepCondition current_step_condition() const;
    void adjust_current_step_condition(int delta);
    void reset_current_step_condition();
    bool current_step_fill() const;
    void toggle_current_step_fill();
    void record_snapshot(uint8_t slot);
    void record_snapshot_voice(uint8_t slot, float vel = 1.0f, bool latch = false, uint8_t gate_steps = 1, uint32_t capture_tick = 0xFFFFFFFFu);
    void record_note(uint8_t logical_pad, uint8_t midi_note, float vel = 1.0f);
    void record_drum(DrumId id, float vel = 1.0f);
    void record_param(ParamId id, float value);
    void record_fx(uint8_t fx_id, bool on);
    void record_arp();
    void panic_restore(RingBuffer<SequencerEvent, 128>& queue);

    uint32_t pending_overflow_count() const { return pending_overflow_count_; }
    uint32_t event_queue_drop_count() const { return event_queue_drop_count_; }
    uint8_t pending_high_water() const { return pending_high_water_; }
    uint8_t event_queue_high_water() const { return event_queue_high_water_; }
    uint8_t max_events_per_step() const { return max_events_per_step_; }

    uint32_t    current_tick()  const { return tick_; }
    ClockSource clock_source()  const { return clock_src_; }
    PlayState   play_state()    const { return play_state_; }
    float       get_bpm()       const { return bpm_; }
    float       get_swing()     const { return swing_; }
    float       get_groove_amount() const { return swing_; }

    void tick(RingBuffer<SequencerEvent, 128>& queue);
    void start_random_chain(RandomizeMode mode);
    void mutate_generative_sequence(float amount, bool wild);
    bool is_random_chain_active() const { return generative_enabled_; }
    void set_clock_out(ClockOut* co) { clock_out_ = co; }
    // Publish transport and current step state to StateManager for audio-side consumers.
    void set_state_manager(StateManager* sm) { state_mgr_ = sm; }

private:
    static constexpr uint8_t  MAX_SEQ_STEPS = 64;
    static constexpr uint32_t STEP_TICKS    = INT_PPQN / 4;  // 16th-note timeline

    struct PendingEvent {
        uint32_t due_tick = 0;
        SequencerEvent ev{};
        bool active = false;
    };

    void advance_tick(RingBuffer<SequencerEvent, 128>& queue);
    void flush_pending_events(RingBuffer<SequencerEvent, 128>& queue);
    bool enqueue_event(RingBuffer<SequencerEvent, 128>& queue, const SequencerEvent& ev, bool count_step_load = false);
    void update_queue_metrics(const RingBuffer<SequencerEvent, 128>& queue);
    void update_pending_metrics();
    void reset_runtime_playback_state(bool clear_pending);
    void schedule_event(const SequencerEvent& ev, uint32_t delay_ticks);
    uint8_t groove_delay_ticks_for_step(uint8_t step_index, bool drum_lane, bool note_lane) const;
    float groove_velocity_scale_for_step(uint8_t step_index, bool drum_lane, bool note_lane) const;
    uint32_t current_tick_interval_us(uint32_t tick_index) const;
    void run_generative_step(RingBuffer<SequencerEvent, 128>& queue);
    uint32_t rng_next();
    float rand01();
    SequenceStep& current_record_step();
    SequenceStep& record_step_for_tick(uint32_t capture_tick);
    uint8_t resolve_record_step_index(uint32_t capture_tick) const;
    void normalize_sequence_length();
    const SequenceStep& current_play_step_ref() const;
    void clear_sequence();
    void clear_step(SequenceStep& step);
    bool step_is_empty(const SequenceStep& step) const;
    uint8_t quantize_param(float value) const;
    float dequantize_param(uint8_t value) const;
    void emit_step(const SequenceStep& step, RingBuffer<SequencerEvent, 128>& queue, uint8_t base_delay_ticks = 0);
    void emit_param_lock(ParamId id, float value, RingBuffer<SequencerEvent, 128>& queue);
    uint8_t next_step_index(uint8_t current);
    bool should_trigger_step(const SequenceStep& step);
    uint8_t effective_ratchet_for_step(const SequenceStep& step) const;
    uint32_t steps_per_cycle() const;
    bool is_cycle_wrap(uint8_t previous_step, uint8_t next_step) const;
    bool is_pendulum_cycle_completion(uint8_t previous_step, uint8_t next_step) const;

    ClockSource clock_src_  = ClockSource::INT;
    PlayState   play_state_ = PlayState::STOPPED;
    float       bpm_        = 120.0f;
    float       swing_      = 0.0f;
    uint32_t    tick_       = 0;
    uint64_t    last_tick_us_   = 0;
    uint32_t    tick_period_us_ = 0;

    volatile uint16_t ticks_pending_ = 0;
    ClockOut*         clock_out_     = nullptr;
    StateManager*     state_mgr_      = nullptr;
    uint32_t          debug_counter_ = 0;
    bool              generative_enabled_ = false;
    bool              generative_wild_    = false;
    uint8_t           chain_remaining_    = 0;
    uint8_t           current_slot_       = 0;
    uint8_t           slot_span_          = 1;
    uint32_t          step_len_ticks_     = INT_PPQN / 2;
    float             kick_prob_          = 0.65f;
    float             snare_prob_         = 0.26f;
    float             hat_prob_           = 0.58f;
    float             stutter_on_prob_    = 0.00f;
    float             stutter_off_prob_   = 0.15f;
    float             slot_jump_prob_     = 0.12f;
    float             slot_trigger_prob_  = 1.00f;
    float             retrig_prob_        = 0.08f;
    uint8_t           bars_per_phrase_    = 1;
    uint32_t          random_state_       = 0x2468ACE1u;

    SequenceStep      sequence_[MAX_SEQ_STEPS] = {};
    uint8_t           seq_length_         = 0;
    uint8_t           write_step_         = 0;
    uint8_t           play_step_          = 0;
    uint8_t           last_step_          = 0xFF;
    uint8_t           arp_play_slot_      = 0;
    uint8_t           active_fx_mask_     = 0;
    bool              sequence_valid_     = false;
    bool              manual_step_write_  = false;
    bool              armed_record_       = false;
    uint8_t           preroll_steps_left_ = 0;
    SequenceStep      page_copy_[8] = {};
    bool              page_copy_valid_ = false;
    uint8_t           active_note_mask_ = 0;
    uint8_t           active_note_midi_[8] = {};
    bool              step_preemitted_ = false;
    GrooveTemplate    groove_template_ = GrooveTemplate::STRAIGHT;
    PlayDirection     play_direction_ = PlayDirection::FORWARD;
    int8_t            pendulum_dir_ = 1;
    IDMVariant        idm_variant_ = IDMVariant::TIGHT;
    float             density_ = 0.50f;
    float             chaos_ = 0.25f;
    uint32_t          sequence_cycle_count_ = 0;
    bool              pendulum_cycle_armed_ = false;
    uint32_t          step_advance_count_ = 0;
    PendingEvent      pending_[48] = {};
    uint32_t          pending_overflow_count_ = 0;
    uint32_t          event_queue_drop_count_ = 0;
    uint8_t           pending_high_water_ = 0;
    uint8_t           event_queue_high_water_ = 0;
    uint8_t           current_step_event_count_ = 0;
    uint8_t           max_events_per_step_ = 0;
};

