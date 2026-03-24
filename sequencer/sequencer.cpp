// sequencer.cpp — V1.5.0 hybrid sequence capture
#include "sequencer.h"
#include "hardware/sync.h"
#include "../utils/debug_log.h"
#include "pico/stdlib.h"
#include <cstring>

namespace {
inline float clampf_local(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

struct GrooveProfile {
    float timing_bias[8];
    float velocity_bias[8];
};

inline const GrooveProfile& groove_profile(GrooveTemplate tpl) {
    static const GrooveProfile kProfiles[] = {
        {{0.f,0.f,0.f,0.f,0.f,0.f,0.f,0.f}, {0.f,0.f,0.f,0.f,0.f,0.f,0.f,0.f}},
        {{0.f,0.38f,0.f,0.30f,0.f,0.42f,0.f,0.32f}, {0.05f,-0.04f,0.03f,-0.03f,0.06f,-0.05f,0.02f,-0.03f}},
        {{0.f,0.52f,0.f,0.48f,0.f,0.56f,0.f,0.50f}, {0.00f,-0.02f,0.03f,-0.03f,0.01f,-0.01f,0.04f,-0.04f}},
        {{0.f,0.44f,0.08f,0.35f,0.00f,0.50f,0.10f,0.28f}, {0.08f,-0.06f,0.04f,-0.05f,0.10f,-0.08f,0.03f,-0.04f}},
        {{0.f,0.62f,0.f,0.58f,0.f,0.64f,0.f,0.60f}, {0.02f,-0.02f,0.01f,-0.01f,0.02f,-0.02f,0.01f,-0.01f}},
        {{0.f,0.14f,0.58f,0.08f,0.46f,0.22f,0.66f,0.12f}, {0.10f,-0.12f,0.06f,-0.08f,0.08f,-0.10f,0.12f,-0.06f}},
    };
    return kProfiles[static_cast<uint8_t>(tpl) % static_cast<uint8_t>(GrooveTemplate::COUNT)];
}

inline const char* play_direction_name(PlayDirection dir) {
    switch (dir) {
    case PlayDirection::FORWARD: return "FWD";
    case PlayDirection::REVERSE: return "REV";
    case PlayDirection::PENDULUM: return "PEND";
    case PlayDirection::RANDOM: return "RAND";
    default: return "?";
    }
}

inline const char* groove_template_name(GrooveTemplate tpl) {
    switch (tpl) {
        case GrooveTemplate::STRAIGHT: return "STRAIGHT";
        case GrooveTemplate::MPC:      return "MPC";
        case GrooveTemplate::SHUFFLE:  return "SHUFFLE";
        case GrooveTemplate::BROKEN:   return "BROKEN";
        case GrooveTemplate::TRIPLET:  return "TRIPLET";
        case GrooveTemplate::IDM:      return "IDM";
        default:                       return "UNKNOWN";
    }
}
inline const char* idm_variant_name(IDMVariant variant) {
    switch (variant) {
        case IDMVariant::TIGHT:   return "TIGHT";
        case IDMVariant::BROKEN:  return "BROKEN";
        case IDMVariant::SKITTER: return "SKITTER";
        case IDMVariant::MELT:    return "MELT";
        default:                  return "?";
    }
}

struct IDMFlavorProfile {
    float kick_repeat;
    float ghost_snare;
    float hat_retrig;
    float stutter_bias;
    float slot_jump_bias;
    float velocity_lofi;
};

inline IDMFlavorProfile idm_profile(IDMVariant variant) {
    switch (variant) {
        case IDMVariant::TIGHT:   return {0.10f, 0.16f, 0.12f, 0.04f, 0.02f, 0.00f};
        case IDMVariant::BROKEN:  return {0.18f, 0.24f, 0.18f, 0.08f, 0.10f, 0.04f};
        case IDMVariant::SKITTER: return {0.12f, 0.18f, 0.32f, 0.10f, 0.14f, 0.02f};
        case IDMVariant::MELT:    return {0.08f, 0.12f, 0.14f, 0.16f, 0.06f, 0.10f};
        default:                  return {0.10f, 0.16f, 0.12f, 0.04f, 0.02f, 0.00f};
    }
}
}

void Sequencer::init() {
    set_bpm(120.0f);
    swing_ = 0.0f;
    clear_sequence();
    groove_template_ = GrooveTemplate::STRAIGHT;
    idm_variant_ = IDMVariant::TIGHT;
}

void Sequencer::clear_step(SequenceStep& step) {
    step.snapshot_mask = 0;
    step.note_mask = 0;
    step.note_tie_mask = 0;
    step.drum_mask = 0;
    step.snapshot_voice_mask = 0;
    step.snapshot_voice_latch_mask = 0;
    step.fx_hold_mask = 0;
    step.arp_enabled = false;
    step.param_mask = 0;
    step.fill_enabled = false;
    step.condition = static_cast<uint8_t>(StepCondition::ALWAYS);
    std::memset(step.param_values, 0, sizeof(step.param_values));
    std::memset(step.note_midi, 0, sizeof(step.note_midi));
    std::memset(step.snapshot_voice_vel_q7, 0, sizeof(step.snapshot_voice_vel_q7));
    std::memset(step.snapshot_voice_gate_steps, 0, sizeof(step.snapshot_voice_gate_steps));
    step.chance_q8 = 255;
    step.ratchet_count = 1;
    step.micro_timing_ticks = 0;
}

void Sequencer::clear_sequence() {
    for (auto& step : sequence_) clear_step(step);
    seq_length_ = 0;
    write_step_ = 0;
    play_step_ = 0;
    pendulum_dir_ = 1;
    last_step_ = 0xFF;
    arp_play_slot_ = 0;
    active_fx_mask_ = 0;
    active_note_mask_ = 0;
    std::memset(active_note_midi_, 0, sizeof(active_note_midi_));
    sequence_valid_ = false;
    for (auto &p : pending_) p.active = false;
    step_preemitted_ = false;
    sequence_cycle_count_ = 0;
    pendulum_cycle_armed_ = false;
    step_advance_count_ = 0;
}

void Sequencer::normalize_sequence_length() {
    while (seq_length_ > 0 && step_is_empty(sequence_[seq_length_ - 1])) {
        --seq_length_;
    }
    sequence_valid_ = seq_length_ > 0;
    if (seq_length_ == 0) {
        play_step_ = 0;
        if (!manual_step_write_) write_step_ = 0;
    } else if (play_step_ >= seq_length_) {
        play_step_ = 0;
    }
}

bool Sequencer::step_is_empty(const SequenceStep& step) const {
    return step.snapshot_mask == 0 && step.note_mask == 0 && step.note_tie_mask == 0 && step.drum_mask == 0 &&
           step.snapshot_voice_mask == 0 && step.fx_hold_mask == 0 &&
           !step.arp_enabled && !step.fill_enabled && step.condition == static_cast<uint8_t>(StepCondition::ALWAYS) &&
           step.param_mask == 0 && step.chance_q8 == 255 && step.ratchet_count == 1 && step.micro_timing_ticks == 0;
}

uint8_t Sequencer::quantize_param(float value) const {
    value = clampf_local(value, 0.0f, 1.0f);
    return (uint8_t)(value * 255.0f + 0.5f);
}

float Sequencer::dequantize_param(uint8_t value) const {
    return (float)value / 255.0f;
}

SequenceStep& Sequencer::current_record_step() {
    if (play_state_ == PlayState::RECORDING && !manual_step_write_ && has_sequence()) {
        uint8_t target = (last_step_ != 0xFF) ? last_step_ : play_step_;
        if (target >= seq_length_) target = 0;
        write_step_ = target;
    }
    if (write_step_ >= MAX_SEQ_STEPS) write_step_ = MAX_SEQ_STEPS - 1;
    return sequence_[write_step_];
}

uint8_t Sequencer::resolve_record_step_index(uint32_t capture_tick) const {
    uint8_t fallback = write_step_;
    if (play_state_ == PlayState::RECORDING && !manual_step_write_ && has_sequence()) {
        fallback = (last_step_ != 0xFF) ? last_step_ : play_step_;
        if (fallback >= seq_length_) fallback = 0;
    }
    if (capture_tick == 0xFFFFFFFFu || manual_step_write_ || play_state_ != PlayState::RECORDING || !has_sequence()) {
        return fallback;
    }
    if (tick_ <= capture_tick) return fallback;
    const uint32_t delta_ticks = tick_ - capture_tick;
    const uint8_t steps_back = (uint8_t)((delta_ticks + (STEP_TICKS / 2u)) / STEP_TICKS);
    return (steps_back > fallback) ? 0u : (uint8_t)(fallback - steps_back);
}

SequenceStep& Sequencer::record_step_for_tick(uint32_t capture_tick) {
    const uint8_t idx = resolve_record_step_index(capture_tick);
    write_step_ = idx;
    return sequence_[idx];
}


bool Sequencer::step_has_snapshot(uint8_t step) const {
    return step < MAX_SEQ_STEPS && !step_is_empty(sequence_[step]) && (sequence_[step].snapshot_mask != 0);
}

bool Sequencer::step_has_note(uint8_t step) const {
    return step < MAX_SEQ_STEPS && !step_is_empty(sequence_[step]) && ((sequence_[step].note_mask | sequence_[step].note_tie_mask) != 0);
}

bool Sequencer::step_has_drum(uint8_t step) const {
    return step < MAX_SEQ_STEPS && !step_is_empty(sequence_[step]) && sequence_[step].drum_mask != 0;
}

bool Sequencer::step_has_snapshot_voice(uint8_t step) const {
    return step < MAX_SEQ_STEPS && !step_is_empty(sequence_[step]) && sequence_[step].snapshot_voice_mask != 0;
}

bool Sequencer::step_has_motion(uint8_t step) const {
    if (step >= MAX_SEQ_STEPS || step_is_empty(sequence_[step])) return false;
    const SequenceStep& s = sequence_[step];
    return s.param_mask != 0 || s.fx_hold_mask != 0 || s.arp_enabled || s.chance_q8 < 255 || s.ratchet_count > 1 || s.micro_timing_ticks != 0;
}

bool Sequencer::step_has_any(uint8_t step) const {
    return step < MAX_SEQ_STEPS && !step_is_empty(sequence_[step]);
}


float Sequencer::current_step_chance() const {
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return 1.0f;
    return (float)sequence_[idx].chance_q8 / 255.0f;
}

void Sequencer::adjust_current_step_chance(int delta) {
    if (!manual_step_write_ && !is_overdub() && !has_sequence()) return;
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return;
    int value = (int)sequence_[idx].chance_q8 + delta * 12;
    if (value < 13) value = 13;
    if (value > 255) value = 255;
    sequence_[idx].chance_q8 = (uint8_t)value;
    if ((idx + 1u) > seq_length_) seq_length_ = idx + 1u;
    sequence_valid_ = seq_length_ > 0;
    LOG_AUDIO("SEQ: step chance step=%u -> %u%%", (unsigned)idx, (unsigned)((value * 100 + 127) / 255));
}

void Sequencer::reset_current_step_chance() {
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return;
    sequence_[idx].chance_q8 = 255;
    LOG_AUDIO("SEQ: step chance reset step=%u", (unsigned)idx);
}

uint8_t Sequencer::current_step_ratchet() const {
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return 1;
    return sequence_[idx].ratchet_count < 1 ? 1 : sequence_[idx].ratchet_count;
}

void Sequencer::adjust_current_step_ratchet(int delta) {
    if (!manual_step_write_ && !is_overdub() && !has_sequence()) return;
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return;
    int value = (int)sequence_[idx].ratchet_count + delta;
    if (value < 1) value = 1;
    if (value > 4) value = 4;
    sequence_[idx].ratchet_count = (uint8_t)value;
    if ((idx + 1u) > seq_length_) seq_length_ = idx + 1u;
    sequence_valid_ = seq_length_ > 0;
    LOG_AUDIO("SEQ: step ratchet step=%u -> %u", (unsigned)idx, (unsigned)value);
}

void Sequencer::reset_current_step_ratchet() {
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return;
    sequence_[idx].ratchet_count = 1;
    LOG_AUDIO("SEQ: step ratchet reset step=%u", (unsigned)idx);
}

int8_t Sequencer::current_step_microtiming() const {
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return 0;
    return sequence_[idx].micro_timing_ticks;
}

void Sequencer::adjust_current_step_microtiming(int delta) {
    if (!manual_step_write_ && !is_overdub() && !has_sequence()) return;
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return;
    int value = (int)sequence_[idx].micro_timing_ticks + delta;
    if (value < -3) value = -3;
    if (value > 3) value = 3;
    sequence_[idx].micro_timing_ticks = (int8_t)value;
    if ((idx + 1u) > seq_length_) seq_length_ = idx + 1u;
    sequence_valid_ = seq_length_ > 0;
    LOG_AUDIO("SEQ: step micro step=%u -> %d ticks", (unsigned)idx, value);
}

void Sequencer::reset_current_step_microtiming() {
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return;
    sequence_[idx].micro_timing_ticks = 0;
    LOG_AUDIO("SEQ: step micro reset step=%u", (unsigned)idx);
}

StepCondition Sequencer::current_step_condition() const {
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return StepCondition::ALWAYS;
    const uint8_t raw = sequence_[idx].condition;
    return raw < static_cast<uint8_t>(StepCondition::COUNT) ? static_cast<StepCondition>(raw) : StepCondition::ALWAYS;
}

void Sequencer::adjust_current_step_condition(int delta) {
    if (!manual_step_write_ && !is_overdub() && !has_sequence()) return;
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS || delta == 0) return;
    int value = (int)sequence_[idx].condition + (delta > 0 ? 1 : -1);
    const int count = static_cast<int>(StepCondition::COUNT);
    while (value < 0) value += count;
    while (value >= count) value -= count;
    sequence_[idx].condition = (uint8_t)value;
    if ((idx + 1u) > seq_length_) seq_length_ = idx + 1u;
    sequence_valid_ = seq_length_ > 0;
    LOG_AUDIO("SEQ: step condition step=%u -> %u", (unsigned)idx, (unsigned)value);
}

void Sequencer::reset_current_step_condition() {
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return;
    sequence_[idx].condition = static_cast<uint8_t>(StepCondition::ALWAYS);
    LOG_AUDIO("SEQ: step condition reset step=%u", (unsigned)idx);
}

bool Sequencer::current_step_fill() const {
    const uint8_t idx = current_edit_step_index();
    return idx < MAX_SEQ_STEPS ? sequence_[idx].fill_enabled : false;
}

void Sequencer::toggle_current_step_fill() {
    if (!manual_step_write_ && !is_overdub() && !has_sequence()) return;
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return;
    sequence_[idx].fill_enabled = !sequence_[idx].fill_enabled;
    if ((idx + 1u) > seq_length_) seq_length_ = idx + 1u;
    sequence_valid_ = seq_length_ > 0;
    LOG_AUDIO("SEQ: step fill step=%u -> %s", (unsigned)idx, sequence_[idx].fill_enabled ? "ON" : "OFF");
}
uint8_t Sequencer::visible_page_base() const {
    uint8_t anchor = 0;
    if (manual_step_write_) anchor = write_step_;
    else if (play_state_ == PlayState::PLAYING || play_state_ == PlayState::RECORDING) {
        anchor = (last_step_ != 0xFF) ? last_step_ : play_step_;
    }
    return (uint8_t)((anchor / 8u) * 8u);
}
uint8_t Sequencer::current_edit_step_index() const {
    if (manual_step_write_) return write_step_;
    if (play_state_ == PlayState::RECORDING && has_sequence()) {
        uint8_t target = (last_step_ != 0xFF) ? last_step_ : play_step_;
        return target < seq_length_ ? target : 0;
    }
    if (has_sequence()) {
        uint8_t target = (last_step_ != 0xFF) ? last_step_ : play_step_;
        return target < seq_length_ ? target : 0;
    }
    return 0;
}
const SequenceStep& Sequencer::current_play_step_ref() const {
    static SequenceStep empty;
    if (!has_sequence() || play_step_ >= seq_length_) return empty;
    return sequence_[play_step_];
}

void Sequencer::arm_record() {
    clear_sequence();
    manual_step_write_ = false;
    armed_record_ = true;
    preroll_steps_left_ = 16;
    tick_ = 0;
    play_step_ = 0;
    step_preemitted_ = false;
    play_state_ = (clock_src_ == ClockSource::EXT) ? PlayState::ARMED : PlayState::PLAYING;
    sequence_cycle_count_ = 0;
    pendulum_cycle_armed_ = false;
    step_advance_count_ = 0;
    last_tick_us_ = time_us_64();
    LOG_AUDIO("SEQ: armed record preroll=%u", (unsigned)preroll_steps_left_);
}

uint32_t Sequencer::rng_next() {
    random_state_ ^= (random_state_ << 13);
    random_state_ ^= (random_state_ >> 17);
    random_state_ ^= (random_state_ << 5);
    return random_state_;
}

float Sequencer::rand01() {
    return (float)(rng_next() & 0x00FFFFFFu) / 16777215.0f;
}

void Sequencer::mutate_generative_sequence(float amount, bool wild) {
    amount = clampf_local(amount, 0.0f, 1.0f);

    random_state_ ^= (uint32_t)(tick_ + 1u) * 0x9E3779B9u;
    random_state_ ^= wild ? 0xA5A55A5Au : 0x3C6EF372u;

    generative_enabled_ = true;
    generative_wild_    = wild;

    const float density_bias = density_ * 0.35f;
    const float chaos_bias = chaos_ * 0.30f;
    const float energy = (wild ? (0.45f + amount * 0.40f)
                               : (0.18f + amount * 0.22f)) + density_bias;

    chain_remaining_ = wild ? (uint8_t)(12 + (uint8_t)(amount * 18.0f))
                            : (uint8_t)(6 + (uint8_t)(amount * 8.0f));

    if (wild) {
        step_len_ticks_ = (amount > 0.82f) ? (INT_PPQN / 8)
                                           : (amount > 0.42f ? (INT_PPQN / 4) : (INT_PPQN / 2));
    } else {
        if (amount < 0.30f) {
            step_len_ticks_ = INT_PPQN / 2;
        } else if (amount < 0.78f) {
            step_len_ticks_ = INT_PPQN / 4;
        } else {
            step_len_ticks_ = INT_PPQN / 2;
        }
    }
    if (step_len_ticks_ == 0) step_len_ticks_ = INT_PPQN / 2;

    slot_span_         = (uint8_t)(1 + (wild ? (int)(amount * 3.0f) : (amount > 0.60f ? 1 : 0)));
    bars_per_phrase_   = wild ? (amount > 0.55f ? 1u : 2u)
                              : (amount < 0.35f ? 4u : (amount < 0.72f ? 2u : 1u));
    const float idm_slot_bias = (groove_template_ == GrooveTemplate::IDM) ? idm_profile(idm_variant_).slot_jump_bias : 0.0f;
    slot_jump_prob_    = wild ? (0.12f + amount * 0.36f + chaos_bias * 0.40f + idm_slot_bias) : (0.03f + amount * 0.10f + chaos_bias * 0.16f + idm_slot_bias * 0.5f);
    slot_trigger_prob_ = wild ? 1.00f : (0.90f + amount * 0.08f);
    retrig_prob_       = wild ? (0.05f + amount * 0.22f + density_bias * 0.20f) : (0.01f + amount * 0.06f + density_bias * 0.08f + chaos_bias * 0.06f);
    if (groove_template_ == GrooveTemplate::IDM) retrig_prob_ += idm_profile(idm_variant_).hat_retrig * 0.25f;

    kick_prob_         = wild ? (0.56f + energy * 0.22f) : (0.56f + energy * 0.12f);
    snare_prob_        = wild ? (0.18f + energy * 0.28f) : (0.10f + energy * 0.12f);
    hat_prob_          = wild ? (0.42f + energy * 0.36f) : (0.34f + energy * 0.16f);
    stutter_on_prob_   = wild ? (0.04f + amount * 0.20f) : (0.00f + amount * 0.04f);
    stutter_off_prob_  = wild ? (0.12f + (1.0f - amount) * 0.10f)
                              : (0.16f + (1.0f - amount) * 0.06f);

    const float slot_move_p = wild ? (0.20f + amount * 0.28f)
                                   : (0.05f + amount * 0.08f);
    if (rand01() < slot_move_p) {
        if (wild && rand01() < 0.35f) {
            current_slot_ = (uint8_t)(rng_next() % 8u);
        } else {
            int jump = 1 + (int)(rng_next() % (uint32_t)(slot_span_ > 0 ? slot_span_ : 1));
            if (rand01() < 0.5f) jump = -jump;
            int next = (int)current_slot_ + jump;
            while (next < 0) next += 8;
            while (next > 7) next -= 8;
            current_slot_ = (uint8_t)next;
        }
    }

    if (play_state_ == PlayState::STOPPED) play();

    LOG_AUDIO("SEQ: mutate sequence amount=%.2f mode=%s len=%u step=%lu slot=%u span=%u jump=%.2f retrig=%.2f",
              amount, wild ? "WILD" : "CTRL",
              (unsigned)chain_remaining_,
              (unsigned long)step_len_ticks_,
              (unsigned)current_slot_,
              (unsigned)slot_span_,
              slot_jump_prob_,
              retrig_prob_);
}

void Sequencer::start_random_chain(RandomizeMode mode) {
    const bool wild = (mode == RandomizeMode::WILD);
    if (wild) {
        if (groove_template_ == GrooveTemplate::STRAIGHT || groove_template_ == GrooveTemplate::MPC) {
            set_groove_template(GrooveTemplate::BROKEN);
        }
        mutate_generative_sequence(0.85f, true);
    } else {
        set_groove_template(GrooveTemplate::IDM);
        if (swing_ < 0.28f) set_swing(0.28f);
        mutate_generative_sequence(0.58f, false);
    }
    if (groove_template_ == GrooveTemplate::IDM) {
        LOG_AUDIO("SEQ: random chain %s len=%u start_slot=%u groove=%s/%s swing=%.2f",
                  wild ? "WILD" : "CTRL",
                  (unsigned)chain_remaining_,
                  (unsigned)current_slot_,
                  groove_template_name(groove_template_),
                  idm_variant_name(idm_variant_),
                  swing_);
    } else {
        LOG_AUDIO("SEQ: random chain %s len=%u start_slot=%u groove=%s swing=%.2f",
                  wild ? "WILD" : "CTRL",
                  (unsigned)chain_remaining_,
                  (unsigned)current_slot_,
                  groove_template_name(groove_template_),
                  swing_);
    }
}

void Sequencer::run_generative_step(RingBuffer<SequencerEvent, 128>& queue) {
    if (!generative_enabled_ || has_sequence()) return;
    const uint32_t step_len = step_len_ticks_ ? step_len_ticks_ : (INT_PPQN / 2);
    const uint32_t bar_len  = INT_PPQN * 4;
    const uint32_t phrase_bars = bars_per_phrase_ ? bars_per_phrase_ : 1u;
    const uint32_t phrase_len = bar_len * phrase_bars;

    if ((tick_ % step_len) != 0) return;

    const bool phrase_edge = ((tick_ % phrase_len) == 0);
    if (phrase_edge || rand01() < slot_jump_prob_) {
        if (generative_wild_) {
            if (rand01() < 0.55f) {
                current_slot_ = (uint8_t)(rng_next() % 8u);
            } else {
                int jump = 1 + (int)(rng_next() % (uint32_t)(slot_span_ > 0 ? slot_span_ : 1));
                if (rand01() < 0.5f) jump = -jump;
                int next = (int)current_slot_ + jump;
                while (next < 0) next += 8;
                while (next > 7) next -= 8;
                current_slot_ = (uint8_t)next;
            }
        } else {
            int jump = 1 + (int)(rng_next() % (uint32_t)(slot_span_ > 0 ? slot_span_ : 1));
            if (rand01() < 0.5f) jump = -jump;
            int next = (int)current_slot_ + jump;
            while (next < 0) next += 8;
            while (next > 7) next -= 8;
            current_slot_ = (uint8_t)next;
        }
    }

    if (rand01() < slot_trigger_prob_) {
        enqueue_event(queue, {tick_, EVT_PAD_TRIGGER, current_slot_, 1.0f}, true);
        if (rand01() < retrig_prob_) {
            enqueue_event(queue, {tick_, EVT_PAD_TRIGGER, current_slot_, 0.72f}, true);
        }
    }

    const bool idm_mode = (groove_template_ == GrooveTemplate::IDM);
    const IDMFlavorProfile idm = idm_profile(idm_variant_);
    const float density_boost = density_ * 0.28f;
    const float chaos_boost = chaos_ * 0.22f;
    const bool on_quarter = ((tick_ % INT_PPQN) == 0);
    const bool on_backbeat = ((tick_ % (INT_PPQN * 2)) == (INT_PPQN / 2));
    const bool offbeat = ((tick_ % INT_PPQN) == (INT_PPQN / 2));
    const bool ghost_pos = ((tick_ % INT_PPQN) == (INT_PPQN / 4)) || ((tick_ % INT_PPQN) == ((INT_PPQN * 3) / 4));
    const bool phrase_fill = idm_mode && phrase_edge && rand01() < (0.40f + density_boost * 0.30f + chaos_boost * 0.20f);
    const float melt_vel = (idm_mode && idm_variant_ == IDMVariant::MELT) ? (0.82f - chaos_boost * 0.10f - idm.velocity_lofi) : 1.0f;

    if (on_quarter || rand01() < kick_prob_ * (idm_mode ? (0.24f + density_boost * 0.12f) : (0.30f + density_boost * 0.10f))) {
        enqueue_event(queue, {tick_, EVT_DRUM_HIT, (uint8_t)DRUM_KICK, melt_vel}, true);
        if (idm_mode && rand01() < (idm.kick_repeat + density_boost * 0.18f + chaos_boost * 0.10f)) {
            schedule_event({tick_, EVT_DRUM_HIT, (uint8_t)DRUM_KICK, 0.74f - idm.velocity_lofi * 0.20f}, INT_PPQN / 8);
        }
    }
    if (phrase_fill) {
        schedule_event({tick_, EVT_DRUM_HIT, (uint8_t)DRUM_KICK, 0.66f}, INT_PPQN / 16);
    }
    if (on_backbeat || (idm_mode && ghost_pos) || rand01() < snare_prob_ * (idm_mode ? (0.30f + density_boost * 0.16f + idm.ghost_snare * 0.20f) : (0.22f + density_boost * 0.08f))) {
        const float snare_vel = (idm_mode && ghost_pos && !on_backbeat) ? (0.56f - idm.velocity_lofi * 0.20f) : melt_vel;
        enqueue_event(queue, {tick_, EVT_DRUM_HIT, (uint8_t)DRUM_SNARE, snare_vel}, true);
        if (idm_mode && phrase_fill && rand01() < (0.50f + chaos_boost * 0.20f)) {
            schedule_event({tick_, EVT_DRUM_HIT, (uint8_t)DRUM_SNARE, 0.48f}, INT_PPQN / 16);
        }
    }
    if (rand01() < (hat_prob_ + density_boost * 0.20f) || (idm_mode && offbeat && rand01() < (0.70f + density_boost * 0.12f))) {
        const float hat_vel = generative_wild_ ? 0.90f : (idm_mode ? (0.84f - idm.velocity_lofi * 0.16f) : 0.78f);
        enqueue_event(queue, {tick_, EVT_DRUM_HIT, (uint8_t)DRUM_HAT, hat_vel}, true);
        if (idm_mode && rand01() < (idm.hat_retrig + density_boost * 0.24f + chaos_boost * 0.12f)) {
            schedule_event({tick_, EVT_DRUM_HIT, (uint8_t)DRUM_HAT, 0.52f - idm.velocity_lofi * 0.12f}, INT_PPQN / 16);
            if (idm_variant_ == IDMVariant::SKITTER && rand01() < (0.30f + density_boost * 0.20f)) {
                schedule_event({tick_, EVT_DRUM_HIT, (uint8_t)DRUM_HAT, 0.40f}, INT_PPQN / 32);
            }
        }
    }

    if (rand01() < (idm_mode ? (stutter_on_prob_ + 0.05f + chaos_boost * 0.20f + idm.stutter_bias) : (stutter_on_prob_ + chaos_boost * 0.12f))) {
        enqueue_event(queue, {tick_, EVT_FX_ON, (uint8_t)FX_BEAT_REPEAT, 1.0f}, true);
    } else if (rand01() < (stutter_off_prob_ + (idm_mode && idm_variant_ == IDMVariant::MELT ? 0.05f : 0.0f))) {
        enqueue_event(queue, {tick_, EVT_FX_OFF, (uint8_t)FX_BEAT_REPEAT, 0.0f}, true);
    }

    if (chain_remaining_ > 0) --chain_remaining_;
    if (chain_remaining_ == 0) {
        generative_enabled_ = false;
        LOG_AUDIO("SEQ: random chain finished");
    }
}

void Sequencer::set_bpm(float bpm) {
    bpm_            = bpm;
    tick_period_us_ = (uint32_t)(60000000.0f / (bpm_ * INT_PPQN));
}

void Sequencer::set_swing(float swing) {
    if (swing < 0.0f) swing = 0.0f;
    if (swing > 0.65f) swing = 0.65f;
    swing_ = swing;
}

void Sequencer::set_groove_template(GrooveTemplate tpl) {
    groove_template_ = static_cast<GrooveTemplate>(static_cast<uint8_t>(tpl) % static_cast<uint8_t>(GrooveTemplate::COUNT));
    LOG_AUDIO("SEQ: groove template=%s", groove_template_name(groove_template_));
}

void Sequencer::adjust_groove_template(int delta) {
    if (delta == 0) return;
    int t = static_cast<int>(groove_template_) + (delta > 0 ? 1 : -1);
    const int count = static_cast<int>(GrooveTemplate::COUNT);
    while (t < 0) t += count;
    while (t >= count) t -= count;
    set_groove_template(static_cast<GrooveTemplate>(t));
}

void Sequencer::set_play_direction(PlayDirection dir) {
    play_direction_ = static_cast<PlayDirection>(static_cast<uint8_t>(dir) % static_cast<uint8_t>(PlayDirection::COUNT));
    pendulum_dir_ = 1;
    pendulum_cycle_armed_ = false;
    LOG_AUDIO("SEQ: direction=%s", play_direction_name(play_direction_));
}

void Sequencer::adjust_play_direction(int delta) {
    if (delta == 0) return;
    int t = static_cast<int>(play_direction_) + (delta > 0 ? 1 : -1);
    const int count = static_cast<int>(PlayDirection::COUNT);
    while (t < 0) t += count;
    while (t >= count) t -= count;
    set_play_direction(static_cast<PlayDirection>(t));
}

void Sequencer::set_idm_variant(IDMVariant variant) {
    idm_variant_ = static_cast<IDMVariant>(static_cast<uint8_t>(variant) % static_cast<uint8_t>(IDMVariant::COUNT));
    LOG_AUDIO("SEQ: idm variant=%s", idm_variant_name(idm_variant_));
}

void Sequencer::adjust_idm_variant(int delta) {
    if (delta == 0) return;
    int t = static_cast<int>(idm_variant_) + (delta > 0 ? 1 : -1);
    const int count = static_cast<int>(IDMVariant::COUNT);
    while (t < 0) t += count;
    while (t >= count) t -= count;
    set_idm_variant(static_cast<IDMVariant>(t));
}

void Sequencer::set_density(float amount) {
    density_ = clampf_local(amount, 0.0f, 1.0f);
}

void Sequencer::set_chaos(float amount) {
    chaos_ = clampf_local(amount, 0.0f, 1.0f);
}

uint8_t Sequencer::next_step_index(uint8_t current) {
    const uint8_t len = seq_length_ ? seq_length_ : 1u;
    switch (play_direction_) {
    case PlayDirection::REVERSE:
        return (current == 0u) ? (uint8_t)(len - 1u) : (uint8_t)(current - 1u);
    case PlayDirection::PENDULUM:
        if (len <= 1u) return 0u;
        if (pendulum_dir_ > 0) {
            if (current >= (uint8_t)(len - 1u)) {
                pendulum_dir_ = -1;
                return (uint8_t)(current > 0u ? current - 1u : 0u);
            }
            return (uint8_t)(current + 1u);
        }
        if (current == 0u) {
            pendulum_dir_ = 1;
            return (uint8_t)(len > 1u ? 1u : 0u);
        }
        return (uint8_t)(current - 1u);
    case PlayDirection::RANDOM:
        return (uint8_t)(rng_next() % len);
    case PlayDirection::FORWARD:
    default:
        return (uint8_t)((current + 1u) % len);
    }
}

uint32_t Sequencer::steps_per_cycle() const {
    const uint32_t len = seq_length_ ? seq_length_ : 1u;
    switch (play_direction_) {
    case PlayDirection::PENDULUM:
        return (len <= 1u) ? 1u : (len - 1u) * 2u;
    case PlayDirection::RANDOM:
    case PlayDirection::REVERSE:
    case PlayDirection::FORWARD:
    default:
        return len;
    }
}

bool Sequencer::is_pendulum_cycle_completion(uint8_t previous_step, uint8_t next_step) const {
    const uint8_t len = seq_length_ ? seq_length_ : 1u;
    if (len <= 1u) return true;
    if (!pendulum_cycle_armed_) return false;
    return previous_step == 0u && next_step == 1u && pendulum_dir_ > 0;
}

bool Sequencer::is_cycle_wrap(uint8_t previous_step, uint8_t next_step) const {
    const uint8_t len = seq_length_ ? seq_length_ : 1u;
    switch (play_direction_) {
    case PlayDirection::FORWARD:
        return previous_step == (uint8_t)(len - 1u) && next_step == 0u;
    case PlayDirection::REVERSE:
        return previous_step == 0u && next_step == (uint8_t)(len - 1u);
    case PlayDirection::PENDULUM:
        return is_pendulum_cycle_completion(previous_step, next_step);
    case PlayDirection::RANDOM:
    default:
        return false;
    }
}

uint32_t Sequencer::current_tick_interval_us(uint32_t tick_index) const {
    if (swing_ <= 0.0f || tick_period_us_ == 0) return tick_period_us_;

    constexpr uint32_t TICKS_PER_EIGHTH = INT_PPQN / 2;
    constexpr uint32_t TICKS_PER_QUARTER = INT_PPQN;

    const uint32_t quarter_pos = tick_index % TICKS_PER_QUARTER;
    const bool first_eighth = quarter_pos < TICKS_PER_EIGHTH;
    const float triplet_blend = swing_ / 3.0f;
    const float multiplier = first_eighth ? (1.0f + triplet_blend)
                                         : (1.0f - triplet_blend);

    const float interval_f = (float)tick_period_us_ * multiplier;
    return (uint32_t)(interval_f < 1.0f ? 1.0f : interval_f);
}

void Sequencer::set_clock_source(ClockSource src) {
    uint32_t s     = save_and_disable_interrupts();
    ticks_pending_ = 0;
    restore_interrupts(s);
    clock_src_ = src;
    LOG_AUDIO("SEQ: source → %s", src == ClockSource::EXT ? "EXT" : "INT");
}

void Sequencer::update_int(uint64_t now_us) {
    if (clock_src_ != ClockSource::INT) return;
    if (play_state_ == PlayState::STOPPED || play_state_ == PlayState::ARMED) return;

    while (true) {
        const uint32_t interval_us = current_tick_interval_us(tick_ + ticks_pending_);
        if (now_us - last_tick_us_ < interval_us) break;

        last_tick_us_ += interval_us;
        if (ticks_pending_ < MAX_TICKS) {
            ticks_pending_++;
        } else {
            break;
        }
    }
}

void Sequencer::on_ext_tick() {
    if (clock_src_ != ClockSource::EXT) return;

    if (play_state_ == PlayState::ARMED) {
        play_state_   = PlayState::PLAYING;
        tick_         = 0;
        last_tick_us_ = time_us_64();
        LOG_AUDIO("SEQ: PLAY — primer pulso EXT");
    }

    if (play_state_ == PlayState::PLAYING || play_state_ == PlayState::RECORDING) {
        uint32_t s  = save_and_disable_interrupts();
        uint16_t nx = ticks_pending_ + INT_MULT;
        ticks_pending_ = (nx > MAX_TICKS) ? MAX_TICKS : nx;
        restore_interrupts(s);
    }
}

void Sequencer::reset_runtime_playback_state(bool clear_pending) {
    tick_ = 0;
    play_step_ = (play_direction_ == PlayDirection::REVERSE && seq_length_ > 0)
                   ? (uint8_t)(seq_length_ - 1u) : 0u;
    pendulum_dir_ = 1;
    last_step_ = 0xFF;
    active_fx_mask_ = 0;
    active_note_mask_ = 0;
    std::memset(active_note_midi_, 0, sizeof(active_note_midi_));
    step_preemitted_ = false;
    sequence_cycle_count_ = 0;
    pendulum_cycle_armed_ = false;
    step_advance_count_ = 0;
    current_step_event_count_ = 0;
    arp_play_slot_ = 0;
    if (clear_pending) {
        for (auto &p : pending_) p.active = false;
    }
}

void Sequencer::play() {
    manual_step_write_ = false;
    reset_runtime_playback_state(true);
    if (clock_src_ == ClockSource::EXT) {
        play_state_ = PlayState::ARMED;
    if (state_mgr_) state_mgr_->set_transport_running(true);
    if (state_mgr_) state_mgr_->set_current_step_index(play_step_);
        if (state_mgr_) state_mgr_->set_transport_running(true);
        if (state_mgr_) state_mgr_->set_current_step_index(play_step_);
        LOG_AUDIO("SEQ: ARMED");
    } else {
        play_state_   = PlayState::PLAYING;
        last_tick_us_ = time_us_64();
        if (state_mgr_) state_mgr_->set_transport_running(true);
        if (state_mgr_) state_mgr_->set_current_step_index(play_step_);
        LOG_AUDIO("SEQ: PLAY INT @ %.1f BPM", bpm_);
    }
}

void Sequencer::rearm() {
    if (clock_src_ != ClockSource::EXT) return;
    play_state_ = PlayState::ARMED;
    uint32_t s  = save_and_disable_interrupts();
    ticks_pending_ = 0;
    restore_interrupts(s);
    reset_runtime_playback_state(true);
    LOG_AUDIO("SEQ: RE-ARMED");
}

void Sequencer::stop() {
    play_state_ = PlayState::STOPPED;
    armed_record_ = false;
    if (state_mgr_) state_mgr_->set_transport_running(false);
    if (state_mgr_) state_mgr_->set_current_step_index(0xFFu);
    reset_runtime_playback_state(true);
    uint32_t s  = save_and_disable_interrupts();
    ticks_pending_ = 0;
    restore_interrupts(s);
    LOG_AUDIO("SEQ: STOP");
}

void Sequencer::rec_toggle() {
    if (play_state_ == PlayState::PLAYING) {
        play_state_ = PlayState::RECORDING;
        if (has_sequence()) {
            write_step_ = (last_step_ != 0xFF) ? last_step_ : play_step_;
            if (write_step_ >= seq_length_) write_step_ = 0;
        }
        LOG_AUDIO("SEQ: overdub ON step=%u", (unsigned)write_step_);
        return;
    }
    if (play_state_ == PlayState::RECORDING) {
        play_state_ = PlayState::PLAYING;
        LOG_AUDIO("SEQ: overdub OFF");
        return;
    }
    if (play_state_ == PlayState::STOPPED) {
        if (!manual_step_write_) {
            clear_sequence();
            manual_step_write_ = true;
            generative_enabled_ = false;
            write_step_ = 0;
            seq_length_ = 0;
            LOG_AUDIO("SEQ: step write armed");
        } else {
            if (!step_is_empty(sequence_[write_step_])) {
                if ((write_step_ + 1) > seq_length_) seq_length_ = write_step_ + 1;
            }
            sequence_valid_ = seq_length_ > 0;
            manual_step_write_ = false;
            play_step_ = 0;
            last_step_ = 0xFF;
            write_step_ = 0;
            LOG_AUDIO("SEQ: step write commit len=%u", (unsigned)seq_length_);
        }
    }
}

void Sequencer::on_manual_advance() {
    if (!manual_step_write_) return;
    if ((write_step_ + 1) > seq_length_) seq_length_ = write_step_ + 1;
    if (write_step_ < (MAX_SEQ_STEPS - 1)) {
        ++write_step_;
    }
    if (seq_length_ < (write_step_ + 1)) seq_length_ = write_step_ + 1;
    LOG_AUDIO("SEQ: write step -> %u", (unsigned)write_step_);
}

void Sequencer::record_snapshot(uint8_t slot) {
    if ((play_state_ != PlayState::RECORDING) && !manual_step_write_) return;
    if (slot >= 8) return;
    current_record_step().snapshot_mask |= (1u << slot);
    if ((write_step_ + 1) > seq_length_) seq_length_ = write_step_ + 1;
}

void Sequencer::record_snapshot_voice(uint8_t slot, float vel, bool latch, uint8_t gate_steps, uint32_t capture_tick) {
    if ((play_state_ != PlayState::RECORDING) && !manual_step_write_) return;
    if (slot >= 8) return;
    SequenceStep& step = record_step_for_tick(capture_tick);
    const uint8_t bit = (uint8_t)(1u << slot);
    step.snapshot_voice_mask |= bit;
    if (latch) step.snapshot_voice_latch_mask |= bit;
    else       step.snapshot_voice_latch_mask &= (uint8_t)~bit;
    vel = clampf_local(vel, 0.0f, 1.0f);
    step.snapshot_voice_vel_q7[slot] = (uint8_t)(vel * 127.0f + 0.5f);
    step.snapshot_voice_gate_steps[slot] = gate_steps == 0 ? 1 : (gate_steps > 8 ? 8 : gate_steps);
    if ((write_step_ + 1) > seq_length_) seq_length_ = write_step_ + 1;
}

void Sequencer::record_note(uint8_t logical_pad, uint8_t midi_note, float) {
    if ((play_state_ != PlayState::RECORDING) && !manual_step_write_) return;
    if (logical_pad >= 8) return;
    SequenceStep& step = current_record_step();
    const uint8_t bit = (uint8_t)(1u << logical_pad);
    if (play_state_ == PlayState::RECORDING && !manual_step_write_ && (step.note_mask & bit) && step.note_midi[logical_pad] == midi_note) {
        step.note_mask &= (uint8_t)~bit;
        step.note_tie_mask &= (uint8_t)~bit;
        step.note_midi[logical_pad] = 0;
    } else {
        step.note_mask |= bit;
        step.note_tie_mask &= (uint8_t)~bit;
        step.note_midi[logical_pad] = midi_note;
    }
    if ((write_step_ + 1) > seq_length_) seq_length_ = write_step_ + 1;
}

void Sequencer::record_drum(DrumId id, float) {
    if ((play_state_ != PlayState::RECORDING) && !manual_step_write_) return;
    if ((uint8_t)id > 2u) return;
    current_record_step().drum_mask |= (1u << (uint8_t)id);
    if ((write_step_ + 1) > seq_length_) seq_length_ = write_step_ + 1;
}

void Sequencer::record_param(ParamId id, float value) {
    if ((play_state_ != PlayState::RECORDING) && !manual_step_write_) return;
    const uint8_t idx = (uint8_t)id;
    if (idx > PARAM_LOCK_LAST) return;
    SequenceStep& step = current_record_step();
    step.param_mask |= (1ull << idx);
    step.param_values[idx] = quantize_param(value);
    if ((write_step_ + 1) > seq_length_) seq_length_ = write_step_ + 1;
}

void Sequencer::record_fx(uint8_t fx_id, bool on) {
    if ((play_state_ != PlayState::RECORDING) && !manual_step_write_) return;
    if (fx_id > 7u) return;
    SequenceStep& step = current_record_step();
    const uint8_t bit = (1u << fx_id);
    if (on) step.fx_hold_mask |= bit;
    else    step.fx_hold_mask &= (uint8_t)~bit;
    if ((write_step_ + 1) > seq_length_) seq_length_ = write_step_ + 1;
}

void Sequencer::record_arp() {
    if ((play_state_ != PlayState::RECORDING) && !manual_step_write_) return;
    current_record_step().arp_enabled = true;
    if ((write_step_ + 1) > seq_length_) seq_length_ = write_step_ + 1;
}

void Sequencer::clear_current_snapshot_layer() {
    if (!has_sequence() && !manual_step_write_) return;
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return;
    sequence_[idx].snapshot_mask = 0;
    normalize_sequence_length();
    LOG_AUDIO("SEQ: clear snapshot layer step=%u", (unsigned)idx);
}

void Sequencer::clear_current_note(uint8_t logical_pad) {
    if ((!has_sequence() && !manual_step_write_) || logical_pad >= 8) return;
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return;
    const uint8_t bit = (uint8_t)(1u << logical_pad);
    sequence_[idx].note_mask &= (uint8_t)~bit;
    sequence_[idx].note_tie_mask &= (uint8_t)~bit;
    sequence_[idx].note_midi[logical_pad] = 0;
    normalize_sequence_length();
    LOG_AUDIO("SEQ: clear note step=%u logical=%u", (unsigned)idx, (unsigned)logical_pad);
}

void Sequencer::toggle_current_note_tie(uint8_t logical_pad) {
    if ((!has_sequence() && !manual_step_write_) || logical_pad >= 8) return;
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return;
    const uint8_t bit = (uint8_t)(1u << logical_pad);
    SequenceStep& step = sequence_[idx];
    if (step.note_mask & bit) {
        step.note_mask &= (uint8_t)~bit;
        step.note_midi[logical_pad] = 0;
    }
    step.note_tie_mask ^= bit;
    if ((idx + 1u) > seq_length_) seq_length_ = idx + 1u;
    sequence_valid_ = seq_length_ > 0;
    LOG_AUDIO("SEQ: toggle note tie step=%u logical=%u -> %s", (unsigned)idx, (unsigned)logical_pad, (step.note_tie_mask & bit) ? "ON" : "OFF");
}

void Sequencer::clear_current_drum_layer() {
    if (!has_sequence() && !manual_step_write_) return;
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return;
    sequence_[idx].drum_mask = 0;
    normalize_sequence_length();
    LOG_AUDIO("SEQ: clear drum layer step=%u", (unsigned)idx);
}

void Sequencer::clear_current_param_lock(ParamId id) {
    if (!has_sequence() && !manual_step_write_) return;
    const uint8_t idx = current_edit_step_index();
    const uint8_t bit = (uint8_t)id;
    if (idx >= MAX_SEQ_STEPS || bit > PARAM_LOCK_LAST) return;
    sequence_[idx].param_mask &= ~(1ull << bit);
    sequence_[idx].param_values[bit] = 0;
    normalize_sequence_length();
    LOG_AUDIO("SEQ: clear param lock step=%u param=%u", (unsigned)idx, (unsigned)bit);
}

void Sequencer::clear_current_step() {
    if (!has_sequence() && !manual_step_write_) return;
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return;
    clear_step(sequence_[idx]);
    if (manual_step_write_) write_step_ = idx;
    normalize_sequence_length();
    LOG_AUDIO("SEQ: clear step=%u", (unsigned)idx);
}

void Sequencer::duplicate_previous_step_into_next() {
    if (!manual_step_write_) return;
    const uint8_t dst = write_step_;
    const uint8_t src = (dst == 0) ? 0 : (uint8_t)(dst - 1u);
    if (dst >= MAX_SEQ_STEPS) return;
    sequence_[dst] = sequence_[src];
    if ((dst + 1u) > seq_length_) seq_length_ = dst + 1u;
    sequence_valid_ = seq_length_ > 0;
    if (write_step_ < (MAX_SEQ_STEPS - 1u)) ++write_step_;
    if ((write_step_ + 1u) > seq_length_) seq_length_ = write_step_ + 1u;
    LOG_AUDIO("SEQ: duplicate step %u -> %u", (unsigned)src, (unsigned)dst);
}

void Sequencer::copy_current_page() {
    const uint8_t base = visible_page_base();
    for (uint8_t i = 0; i < 8; ++i) {
        const uint8_t step = (uint8_t)(base + i);
        page_copy_[i] = (step < MAX_SEQ_STEPS) ? sequence_[step] : SequenceStep{};
    }
    page_copy_valid_ = true;
    LOG_AUDIO("SEQ: copied page base=%u", (unsigned)base);
}

void Sequencer::paste_page_to_current_page() {
    if (!page_copy_valid_) return;
    const uint8_t base = visible_page_base();
    for (uint8_t i = 0; i < 8; ++i) {
        const uint8_t step = (uint8_t)(base + i);
        if (step < MAX_SEQ_STEPS) sequence_[step] = page_copy_[i];
    }
    if ((base + 8u) > seq_length_) seq_length_ = (uint8_t)((base + 8u) > MAX_SEQ_STEPS ? MAX_SEQ_STEPS : (base + 8u));
    normalize_sequence_length();
    LOG_AUDIO("SEQ: pasted page base=%u", (unsigned)base);
}


uint8_t Sequencer::groove_delay_ticks_for_step(uint8_t step_index, bool drum_lane, bool note_lane) const {
    if (swing_ <= 0.001f) return 0;
    const GrooveProfile& gp = groove_profile(groove_template_);
    const uint8_t idx = step_index & 0x07u;
    float lane_scale = drum_lane ? 1.0f : (note_lane ? 0.75f : 0.0f);
    float bias = gp.timing_bias[idx] * lane_scale * (swing_ / 0.65f);
    int ticks = (int)(bias * 6.0f + 0.5f);
    if (ticks < 0) ticks = 0;
    if (ticks > 6) ticks = 6;
    return (uint8_t)ticks;
}

float Sequencer::groove_velocity_scale_for_step(uint8_t step_index, bool drum_lane, bool note_lane) const {
    const GrooveProfile& gp = groove_profile(groove_template_);
    const uint8_t idx = step_index & 0x07u;
    float lane_scale = drum_lane ? 1.0f : (note_lane ? 0.6f : 0.0f);
    float v = 1.0f + gp.velocity_bias[idx] * lane_scale * (0.5f + (swing_ / 0.65f) * 0.5f);
    return clampf_local(v, 0.80f, 1.12f);
}


bool Sequencer::enqueue_event(RingBuffer<SequencerEvent, 128>& queue, const SequencerEvent& ev, bool count_step_load) {
    if (!queue.push(ev)) {
        ++event_queue_drop_count_;
        return false;
    }
    if (count_step_load) {
        if (current_step_event_count_ < 255) ++current_step_event_count_;
        if (current_step_event_count_ > max_events_per_step_) max_events_per_step_ = current_step_event_count_;
    }
    update_queue_metrics(queue);
    return true;
}

void Sequencer::update_queue_metrics(const RingBuffer<SequencerEvent, 128>& queue) {
    const uint8_t used = queue.size();
    if (used > event_queue_high_water_) event_queue_high_water_ = used;
}

void Sequencer::update_pending_metrics() {
    uint8_t used = 0;
    for (const auto &p : pending_) {
        if (p.active) ++used;
    }
    if (used > pending_high_water_) pending_high_water_ = used;
}

void Sequencer::schedule_event(const SequencerEvent& ev, uint32_t delay_ticks) {
    const uint32_t due = tick_ + delay_ticks;
    for (auto &p : pending_) {
        if (!p.active) {
            p.active = true;
            p.due_tick = due;
            p.ev = ev;
            p.ev.tick = due;
            update_pending_metrics();
            return;
        }
    }
    ++pending_overflow_count_;
}

void Sequencer::flush_pending_events(RingBuffer<SequencerEvent, 128>& queue) {
    for (auto &p : pending_) {
        if (!p.active) continue;
        if ((int32_t)(tick_ - p.due_tick) >= 0) {
            if (enqueue_event(queue, p.ev, true)) {
                p.active = false;
            }
        }
    }
    update_pending_metrics();
}

void Sequencer::emit_param_lock(ParamId id, float value, RingBuffer<SequencerEvent, 128>& queue) {
    switch (id) {
    case PARAM_DRUM_DECAY:
        enqueue_event(queue, {tick_, EVT_DRUM_PARAM, (uint8_t)DRUM_PARAM_DECAY, value}, true);
        break;
    case PARAM_DRUM_COLOR:
        enqueue_event(queue, {tick_, EVT_DRUM_PARAM, (uint8_t)DRUM_PARAM_COLOR, value}, true);
        break;
    case PARAM_DUCK_AMOUNT:
        enqueue_event(queue, {tick_, EVT_DRUM_PARAM, (uint8_t)DRUM_PARAM_DUCK, value}, true);
        break;
    default:
        enqueue_event(queue, {tick_, EVT_PARAM_CHANGE, (uint8_t)id, value}, true);
        break;
    }
}

bool Sequencer::should_trigger_step(const SequenceStep& step) {
    const StepCondition cond = (step.condition < static_cast<uint8_t>(StepCondition::COUNT))
        ? static_cast<StepCondition>(step.condition) : StepCondition::ALWAYS;
    switch (cond) {
        case StepCondition::EVERY_2:  if ((sequence_cycle_count_ & 1u) != 0u) return false; break;
        case StepCondition::EVERY_4:  if ((sequence_cycle_count_ & 3u) != 0u) return false; break;
        case StepCondition::RANDOM_50: if (rand01() > 0.5f) return false; break;
        case StepCondition::ALWAYS:
        default: break;
    }
    return (step.chance_q8 >= 255) || ((rng_next() & 0xFFu) <= step.chance_q8);
}

uint8_t Sequencer::effective_ratchet_for_step(const SequenceStep& step) const {
    uint8_t ratchet = step.ratchet_count < 1 ? 1 : (step.ratchet_count > 4 ? 4 : step.ratchet_count);
    if (step.fill_enabled) {
        if (groove_template_ == GrooveTemplate::IDM) {
            ratchet = ratchet < 3 ? 3 : ratchet;
        } else {
            ratchet = ratchet < 2 ? 2 : ratchet;
        }
    }
    return ratchet > 4 ? 4 : ratchet;
}

void Sequencer::emit_step(const SequenceStep& step, RingBuffer<SequencerEvent, 128>& queue, uint8_t base_delay_ticks) {
    const bool step_pass = should_trigger_step(step);
    const uint8_t step_idx = play_step_;
    const uint8_t drum_delay = (uint8_t)(groove_delay_ticks_for_step(step_idx, true, false) + base_delay_ticks);
    const uint8_t note_delay = (uint8_t)(groove_delay_ticks_for_step(step_idx, false, true) + base_delay_ticks);
    float drum_vel = groove_velocity_scale_for_step(step_idx, true, false);
    float note_vel = groove_velocity_scale_for_step(step_idx, false, true);
    if (step.fill_enabled) {
        drum_vel *= (groove_template_ == GrooveTemplate::IDM) ? 1.10f : 1.05f;
        note_vel *= 1.03f;
    }

    const uint8_t ratchet = effective_ratchet_for_step(step);
    const uint8_t ratchet_interval = ratchet > 1 ? (uint8_t)(STEP_TICKS / ratchet) : 0u;

    auto emit_repeated = [&](EventType type, uint8_t target, float value, uint8_t delay, bool count_base) {
        if (delay) schedule_event({tick_, type, target, value}, delay);
        else enqueue_event(queue, {tick_, type, target, value}, count_base);
        if (ratchet > 1 && ratchet_interval > 0u) {
            for (uint8_t r = 1; r < ratchet; ++r) {
                schedule_event({tick_, type, target, value * (r == 1 ? 0.90f : 0.82f)}, (uint32_t)delay + (uint32_t)ratchet_interval * r);
            }
        }
    };

    for (uint8_t logical = 0; logical < 8; ++logical) {
        const uint8_t bit = (uint8_t)(1u << logical);
        const bool had_note = (active_note_mask_ & bit) != 0;
        const bool has_note = step_pass && ((step.note_mask & bit) != 0);
        const bool has_tie  = step_pass && ((step.note_tie_mask & bit) != 0);
        const uint8_t prev_midi = active_note_midi_[logical];
        const uint8_t next_midi = has_note ? step.note_midi[logical] : (has_tie ? prev_midi : 0);

        if (had_note && !has_note && !has_tie) {
            if (prev_midi != 0)  // FIX V1.21 B3: nunca emitir NOTE_OFF(midi=0) = C-1
                enqueue_event(queue, {tick_, EVT_NOTE_OFF, prev_midi, 0.0f}, true);
            active_note_midi_[logical] = 0;
        } else if (had_note && has_note && prev_midi != next_midi) {
            if (prev_midi != 0)  // FIX V1.21 B3: guard legato NOTE_OFF(0)
                enqueue_event(queue, {tick_, EVT_NOTE_OFF, prev_midi, 0.0f}, true);
            emit_repeated(EVT_NOTE_ON, next_midi, 0.85f * note_vel, note_delay, true);
            active_note_midi_[logical] = next_midi;
        } else if (!had_note && has_note) {
            emit_repeated(EVT_NOTE_ON, next_midi, 0.85f * note_vel, note_delay, true);
            active_note_midi_[logical] = next_midi;
        } else if (had_note && has_tie) {
            active_note_midi_[logical] = prev_midi;
        }
    }
    // FIX V1.21 B1: si step_pass=false (chance/condition falló) NO borrar el tracking.
    // Con el 0 anterior, las notas activas perdían su registro sin recibir NOTE_OFF
    // → quedaban sonando hasta panic_restore. Solo actualizar cuando el step se ejecutó.
    if (step_pass) {
        active_note_mask_ = (uint8_t)(step.note_mask | step.note_tie_mask);
    }

    const uint8_t next_fx = step_pass ? step.fx_hold_mask : 0;
    const uint8_t fx_on = (uint8_t)(next_fx & ~active_fx_mask_);
    const uint8_t fx_off = (uint8_t)(active_fx_mask_ & ~next_fx);
    for (uint8_t i = 0; i < 8; ++i) {
        const uint8_t bit = (1u << i);
        if (fx_on & bit)  enqueue_event(queue, {tick_, EVT_FX_ON, i, 1.0f}, true);
        if (fx_off & bit) enqueue_event(queue, {tick_, EVT_FX_OFF, i, 0.0f}, true);
    }
    active_fx_mask_ = next_fx;

    for (uint8_t slot = 0; slot < 8; ++slot) {
        if (step_pass && (step.snapshot_mask & (1u << slot))) {
            emit_repeated(EVT_PAD_TRIGGER, slot, 1.0f, base_delay_ticks, true);
        }
    }

    for (uint8_t slot = 0; slot < 8; ++slot) {
        const uint8_t bit = (uint8_t)(1u << slot);
        if (!step_pass || !(step.snapshot_voice_mask & bit)) continue;

        float voice_vel = (float)step.snapshot_voice_vel_q7[slot] / 127.0f;
        if (voice_vel <= 0.0f) voice_vel = 0.85f;
        voice_vel *= note_vel;
        if (voice_vel > 1.0f) voice_vel = 1.0f;

        const uint8_t gate_steps = step.snapshot_voice_gate_steps[slot] == 0 ? 1 : step.snapshot_voice_gate_steps[slot];
        const uint32_t gate_ticks = (uint32_t)gate_steps * STEP_TICKS;
        const bool latch = (step.snapshot_voice_latch_mask & bit) != 0;

        if (note_delay) schedule_event({tick_, EVT_SNAPSHOT_VOICE_ON, slot, voice_vel}, note_delay);
        else enqueue_event(queue, {tick_, EVT_SNAPSHOT_VOICE_ON, slot, voice_vel}, true);

        if (!latch) {
            schedule_event({tick_, EVT_SNAPSHOT_VOICE_OFF, slot, 0.0f}, (uint32_t)note_delay + gate_ticks);
        }
    }
    if (step_pass && step.arp_enabled) {
        emit_repeated(EVT_PAD_TRIGGER, (uint8_t)(arp_play_slot_ & 0x07u), 0.92f, base_delay_ticks, true);
        arp_play_slot_ = (uint8_t)((arp_play_slot_ + 1u) & 0x07u);
    }
    if (step_pass && (step.drum_mask & (1u << DRUM_KICK)))  emit_repeated(EVT_DRUM_HIT, (uint8_t)DRUM_KICK, 1.0f * drum_vel, drum_delay, true);
    if (step_pass && (step.drum_mask & (1u << DRUM_SNARE))) emit_repeated(EVT_DRUM_HIT, (uint8_t)DRUM_SNARE, 1.0f * drum_vel, drum_delay, true);
    if (step_pass && (step.drum_mask & (1u << DRUM_HAT)))   emit_repeated(EVT_DRUM_HIT, (uint8_t)DRUM_HAT, 0.85f * drum_vel, drum_delay, true);

    for (uint8_t idx = 0; idx <= PARAM_LOCK_LAST; ++idx) {
        if (step_pass && (step.param_mask & (1ull << idx))) {
            emit_param_lock((ParamId)idx, dequantize_param(step.param_values[idx]), queue);
        }
    }
}

void Sequencer::tick(RingBuffer<SequencerEvent, 128>& queue) {
    while (ticks_pending_ > 0) {
        uint32_t s = save_and_disable_interrupts();
        bool have  = ticks_pending_ > 0;
        if (have) ticks_pending_--;
        restore_interrupts(s);
        if (have) advance_tick(queue);
    }
}

void Sequencer::advance_tick(RingBuffer<SequencerEvent, 128>& queue) {
    tick_++;
    flush_pending_events(queue);

    if (clock_out_ && (tick_ % INT_MULT == 0))
        clock_out_->on_tick();

    const bool step_boundary = ((tick_ - 1u) % STEP_TICKS) == 0u;

    if (armed_record_ && step_boundary) {
        const uint8_t count_idx = (uint8_t)(16u - preroll_steps_left_);
        if ((count_idx % 4u) == 0u) {
            enqueue_event(queue, {tick_, EVT_DRUM_HIT, (uint8_t)DRUM_HAT, count_idx == 0u ? 1.0f : 0.92f}, true);
        } else {
            enqueue_event(queue, {tick_, EVT_DRUM_HIT, (uint8_t)DRUM_KICK, 0.45f}, true);
        }
        if (preroll_steps_left_ > 0) --preroll_steps_left_;
        if (preroll_steps_left_ == 0) {
            armed_record_ = false;
            manual_step_write_ = true;
            play_state_ = PlayState::RECORDING;
            if (state_mgr_) state_mgr_->set_transport_running(true);
            if (state_mgr_) state_mgr_->set_current_step_index(play_step_);
            clear_sequence();
            LOG_AUDIO("SEQ: preroll done -> recording");
        }
    }

    if (has_sequence() && (play_state_ == PlayState::PLAYING || play_state_ == PlayState::RECORDING)) {
        if (play_step_ >= seq_length_) play_step_ = 0;
        const SequenceStep& cur = sequence_[play_step_];
        const int8_t micro = cur.micro_timing_ticks;
        const uint32_t pos_in_step = (tick_ - 1u) % STEP_TICKS;
        const uint32_t ticks_to_boundary = (STEP_TICKS - pos_in_step) % STEP_TICKS;
        if (micro < 0 && !step_preemitted_ && ticks_to_boundary == (uint32_t)(-micro)) {
            current_step_event_count_ = 0;
            emit_step(cur, queue, 0);
            last_step_ = play_step_;
            if (state_mgr_) state_mgr_->set_current_step_index(play_step_);
            if (play_state_ == PlayState::RECORDING) write_step_ = play_step_;
            step_preemitted_ = true;
        }
        if (step_boundary) {
            current_step_event_count_ = 0;
            if (!step_preemitted_) {
                emit_step(cur, queue, (uint8_t)(micro > 0 ? micro : 0));
                last_step_ = play_step_;
                if (play_state_ == PlayState::RECORDING) write_step_ = play_step_;
            }
            step_preemitted_ = false;
            const uint8_t prev_step = play_step_;
            play_step_ = next_step_index(play_step_);
            if (play_direction_ == PlayDirection::PENDULUM) {
                const uint8_t len = seq_length_ ? seq_length_ : 1u;
                if (len > 1u && prev_step == (uint8_t)(len - 1u) && pendulum_dir_ < 0) {
                    pendulum_cycle_armed_ = true;
                }
                if (is_cycle_wrap(prev_step, play_step_)) {
                    sequence_cycle_count_++;
                    pendulum_cycle_armed_ = false;
                }
            } else if (play_direction_ == PlayDirection::RANDOM) {
                const uint32_t cycle_steps = steps_per_cycle();
                if (cycle_steps > 0u) {
                    step_advance_count_++;
                    if (step_advance_count_ >= cycle_steps) {
                        step_advance_count_ = 0;
                        sequence_cycle_count_++;
                    }
                }
            } else if (is_cycle_wrap(prev_step, play_step_)) {
                sequence_cycle_count_++;
            }
        }
    }

    run_generative_step(queue);

    if (++debug_counter_ >= INT_PPQN) {
        debug_counter_ = 0;
        update_queue_metrics(queue);
        LOG("SEQ tick=%lu %s %.1fBPM swing=%.2f state=%d pendUsed=%u/%u qUsed=%u/%u drops=%lu pOvf=%lu len=%u step=%u evMax=%u",
            (unsigned long)tick_,
            clock_src_ == ClockSource::EXT ? "EXT" : "INT",
            bpm_, swing_, (int)play_state_,
            (unsigned)pending_high_water_, (unsigned)(sizeof(pending_) / sizeof(pending_[0])),
            (unsigned)queue.size(), (unsigned)RingBuffer<SequencerEvent, 128>::capacity(),
            (unsigned long)event_queue_drop_count_, (unsigned long)pending_overflow_count_,
            (unsigned)seq_length_, (unsigned)play_step_, (unsigned)max_events_per_step_);
    }
}


void Sequencer::panic_restore(RingBuffer<SequencerEvent, 128>& queue) {
    for (uint8_t logical = 0; logical < 8; ++logical) {
        const uint8_t bit = (uint8_t)(1u << logical);
        if ((active_note_mask_ & bit) != 0) {
            const uint8_t midi = active_note_midi_[logical];
            if (midi != 0) {
                enqueue_event(queue, {tick_, EVT_NOTE_OFF, midi, 0.0f}, true);
            }
            active_note_midi_[logical] = 0;
        }
    }
    active_note_mask_ = 0;

    for (uint8_t i = 0; i < 8; ++i) {
        const uint8_t bit = (uint8_t)(1u << i);
        if ((active_fx_mask_ & bit) != 0) {
            enqueue_event(queue, {tick_, EVT_FX_OFF, i, 0.0f}, true);
        }
    }
    active_fx_mask_ = 0;
}
