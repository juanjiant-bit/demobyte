// io/input_router.cpp — BYT3/YUY0 V1.22
//
// CAMBIOS V1.22 vs V1.21:
//
//   FIX: Migración a ControlFrame — process() y todos los handlers reciben
//     const ControlFrame& en vez de CapPadHandler& + AdcHandler& directamente.
//
//   FIX BUG-IR-2 (MEDIO): handle_adc usaba last_shift_ (valor del frame anterior)
//     para detectar cambio de capa. Corregido: usa frame.is_pressed(PAD_SHIFT).
//
//   FIX BUG-IR-3 (MEDIO): handle_snapshots re-leía get_just_released() y
//     to_ms_since_boot() directamente desde hardware. Corregido: usa frame.
//
//   FIX BUG-IR-4/5 (BAJO): snap_arp y handle_drums tenían su propio now_ms
//     interno. Corregidos: usan frame.now_ms.
//
//   FIX BUG-IR-6 (BAJO): find_other_pressed_snapshot_slot era función libre.
//     Migrado a método privado operando sobre ControlFrame.
//
//   FIX morph special case: la condición `i == 6` para el control de snapshot
//     morph reemplazada por `catch_param == PARAM_MORPH` para ser robusta
//     frente a cambios de mapa futuros.
//
//   NOTA sobre pot map: kBaseMap/kShiftMap RESTAURADOS al mapa original V1.21
//     (parámetros del engine bytebeat). La versión anterior de este archivo
//     los había cambiado basándose en un comentario desactualizado, lo cual
//     era incorrecto. Ver resolve_pot_param() para el mapa completo.

#include "../synth/quantizer.h"
#include "input_router.h"
#include "../led/led_controller.h"
#include "../synth/note_mode.h"
#include "../utils/debug_log.h"
#include "pico/stdlib.h"
#include "../hardware/pin_config.h"
#include <cmath>
#include <cstdio>

namespace {
const char* action_text(ActionFeedback action) {
    switch (action) {
        case ActionFeedback::COPY:        return "COPY";
        case ActionFeedback::PASTE:       return "PASTE";
        case ActionFeedback::CLEAR:       return "CLEAR";
        case ActionFeedback::RANDOMIZE:   return "RANDOMIZE";
        case ActionFeedback::SAVE_ARM:    return "SAVE ARM";
        case ActionFeedback::SAVE_OK:     return "SAVE OK";
        case ActionFeedback::STEP_FILL:   return "STEP FILL";
        case ActionFeedback::CONDITION:   return "CONDITION";
        case ActionFeedback::MUTATE_SOFT: return "MUTATE SOFT";
        case ActionFeedback::MUTATE_WILD: return "MUTATE WILD";
        default:                          return "ACTION";
    }
}
} // namespace

// ── Helpers de feedback ────────────────────────────────────────────

void InputRouter::show_action(ActionFeedback action) {
    if (led_ctrl)   led_ctrl->on_action(action);
    if (ui_renderer) ui_renderer->show_action_message(action_text(action));
}

void InputRouter::show_condition_overlay(Sequencer& seq) {
    if (!led_ctrl) return;
    led_ctrl->show_condition_overlay(static_cast<uint8_t>(seq.current_step_condition()),
                                     static_cast<uint8_t>(StepCondition::COUNT));
}

void InputRouter::show_groove_overlay(Sequencer& seq, StateManager&) {
    if (!led_ctrl) return;
    led_ctrl->show_groove_overlay(static_cast<uint8_t>(seq.groove_template()),
                                  static_cast<uint8_t>(GrooveTemplate::COUNT));
}

void InputRouter::show_idm_variant_overlay(Sequencer& seq) {
    if (!led_ctrl) return;
    led_ctrl->show_idm_variant_overlay(static_cast<uint8_t>(seq.idm_variant()),
                                       static_cast<uint8_t>(IDMVariant::COUNT));
}

void InputRouter::show_play_direction_overlay(Sequencer& seq) {
    if (!led_ctrl) return;
    led_ctrl->show_play_direction_overlay(static_cast<uint8_t>(seq.play_direction()),
                                          static_cast<uint8_t>(PlayDirection::COUNT));
}

// ── Helpers de pad ──────────────────────────────────────────────────

bool InputRouter::any_snapshot_pad_pressed(const ControlFrame& frame) const {
    for (uint8_t i = 0; i < NOTE_PAD_COUNT; ++i)
        if (frame.is_pressed(SNAP_PAD_PHYS[i])) return true;
    return false;
}

bool InputRouter::any_drum_pad_pressed(const ControlFrame& frame) const {
    return frame.is_pressed(PAD_KICK) ||
           frame.is_pressed(PAD_SNARE) ||
           frame.is_pressed(PAD_HAT);
}

// FIX BUG-IR-6: migrado de función libre a método privado
uint8_t InputRouter::find_other_pressed_snapshot_slot(const ControlFrame& frame,
                                                        uint8_t exclude_pad) const {
    for (uint8_t slot = 0; slot < NOTE_PAD_COUNT; ++slot) {
        const uint8_t pad = SNAP_PAD_PHYS[slot];
        if (pad == exclude_pad) continue;
        if (frame.is_pressed(pad)) return slot;
    }
    return INVALID_SLOT;
}

// ── Combos ──────────────────────────────────────────────────────────

// FIX BUG-IR-X: now_ms viene del frame, no de una llamada nueva
bool InputRouter::handle_random_combo(const ControlFrame& frame, Sequencer& seq,
                                      StateManager& state, bool shift) {
    const uint32_t now_ms = frame.now_ms;
    const bool random_combo = shift && frame.is_pressed(PAD_KICK) && frame.is_pressed(PAD_SNARE);

    if (random_combo && !random_combo_active_) {
        random_combo_active_ = true;
        random_wild_fired_   = false;
        random_combo_ms_     = now_ms;
    }
    if (random_combo_active_ && random_combo && !random_wild_fired_) {
        const uint32_t held = now_ms - random_combo_ms_;
        if (held >= 1000u) {
            state.randomize_all(RandomizeMode::WILD);
            seq.start_random_chain(RandomizeMode::WILD);
            if (led_ctrl) show_action(ActionFeedback::RANDOMIZE);
            random_wild_fired_ = true;
            return true;
        }
    }
    if (random_combo_active_ && !random_combo) {
        if (!random_wild_fired_) {
            const uint32_t held = now_ms - random_combo_ms_;
            if (held >= RANDOM_GUARD_MS) {
                state.randomize_all(RandomizeMode::CONTROLLED);
                seq.start_random_chain(RandomizeMode::CONTROLLED);
                if (led_ctrl) show_action(ActionFeedback::RANDOMIZE);
                random_combo_active_ = false;
                random_wild_fired_   = false;
                return true;
            }
        }
        random_combo_active_ = false;
        random_wild_fired_   = false;
    }
    return random_combo;
}

bool InputRouter::handle_mutate_combo(const ControlFrame& frame, Sequencer& seq,
                                      StateManager& state) {
    const uint32_t now_ms = frame.now_ms;
    const bool shift_only_kick = frame.is_pressed(PAD_SHIFT) &&
                                 frame.is_pressed(PAD_KICK) &&
                                 !frame.is_pressed(PAD_SNARE) &&
                                 !frame.is_pressed(PAD_REC);

    if (shift_only_kick && !mutate_combo_active_) {
        mutate_combo_active_ = true;
        mutate_wild_fired_   = false;
        mutate_combo_ms_     = now_ms;
    }
    if (mutate_combo_active_ && shift_only_kick && !mutate_wild_fired_) {
        const uint32_t held = now_ms - mutate_combo_ms_;
        if (held >= MUTATE_HOLD_MS) {
            const float amount = state.get_mutate_amount();
            state.mutate_active_snapshot(amount, true);
            seq.mutate_generative_sequence(amount, true);
            mutate_wild_fired_     = true;
            shift_used_for_action_ = true;
            if (led_ctrl) show_action(ActionFeedback::MUTATE_WILD);
        }
    }
    if (mutate_combo_active_ && !shift_only_kick) {
        if (!mutate_wild_fired_) {
            const uint32_t held = now_ms - mutate_combo_ms_;
            if (held < MUTATE_HOLD_MS) {
                const float amount = state.get_mutate_amount();
                state.mutate_active_snapshot(amount, false);
                seq.mutate_generative_sequence(amount, false);
                shift_used_for_action_ = true;
                if (led_ctrl) show_action(ActionFeedback::MUTATE_SOFT);
            }
        }
        mutate_combo_active_ = false;
        mutate_wild_fired_   = false;
    }
    return shift_only_kick || mutate_combo_active_;
}

// ── Queue helper ────────────────────────────────────────────────────

bool InputRouter::enqueue_event(RingBuffer<SequencerEvent, 128>& queue,
                                const SequencerEvent& ev) {
    if (queue.push(ev)) return true;
    ++dropped_queue_events_;
    if ((dropped_queue_events_ & 0x1Fu) == 1u)
        LOG("INPUT: queue overflow drops=%lu", (unsigned long)dropped_queue_events_);
    return false;
}

// ── Context resolution ──────────────────────────────────────────────

PadUiContext InputRouter::resolve_pad_context(bool shift, bool shift_rec) const {
    if (shift_rec)  return PadUiContext::SHIFT_REC_DEEP;
    if (shift)      return PadUiContext::SHIFT_SNAPSHOT;
    if (note_mode_) return PadUiContext::NOTE;
    return PadUiContext::NORMAL;
}

// ── process() ──────────────────────────────────────────────────────

void InputRouter::process(const ControlFrame& frame,
                          Sequencer& seq, StateManager& state,
                          RingBuffer<SequencerEvent, 128>& queue)
{
    const uint16_t just_on  = frame.pads_just_pressed;
    const uint16_t just_off = frame.pads_just_released;
    const bool     shift    = frame.is_pressed(PAD_SHIFT);
    const uint32_t now_ms   = frame.now_ms;

    shift_rec_mode_ = shift && frame.is_pressed(PAD_REC);

    if (!shift) {
        shift_used_for_action_    = false;
        shift_waiting_second_tap_ = false;
    }

    if (seq.is_step_write_mode() && (just_on & (1u << PAD_SHIFT))) {
        const bool interaction_busy =
            any_snapshot_pad_pressed(frame) || any_drum_pad_pressed(frame) ||
            frame.is_pressed(PAD_PLAY) || frame.is_pressed(PAD_REC) ||
            frame.is_pressed(PAD_MUTE);

        shift_used_for_action_ = false;
        if (!interaction_busy && shift_waiting_second_tap_ &&
            (now_ms - last_shift_tap_ms_) <= 320u) {
            seq.clear_current_step();
            if (led_ctrl) show_action(ActionFeedback::CLEAR);
            shift_used_for_action_    = true;
            shift_waiting_second_tap_ = false;
        } else if (!interaction_busy) {
            shift_waiting_second_tap_ = true;
            last_shift_tap_ms_        = now_ms;
        } else {
            shift_waiting_second_tap_ = false;
        }
    }

    handle_encoder(frame, seq, state);

    last_shift_ = shift;
    handle_random_combo(frame, seq, state, shift);
    handle_mutate_combo(frame, seq, state);
    handle_transport(frame, seq, state, shift, queue);

    if (shift && (just_on & (1u << PAD_MUTE))) {
        const bool seq_edit       = seq.is_step_write_mode() || seq.is_overdub();
        const bool shift_rec      = shift_rec_mode_;
        const bool pad_action_busy = any_snapshot_pad_pressed(frame) || any_drum_pad_pressed(frame);

        if (shift_rec && seq_edit && !pad_action_busy) {
            seq.clear_current_step();
            if (led_ctrl) show_action(ActionFeedback::CLEAR);
            shift_used_for_action_ = true;
        } else if (!pad_action_busy) {
            if (seq.play_state() == PlayState::PLAYING && !seq.is_step_write_mode()) {
                seq.panic_restore(queue);
            } else {
                state.set_env_loop(!state.get_env_loop());
                if (led_ctrl) led_ctrl->on_env_loop_toggle(state.get_env_loop());
                LOG_AUDIO("INPUT: Env Loop %s", state.get_env_loop() ? "ON" : "OFF");
            }
            shift_used_for_action_ = true;
        }
    }

    switch (resolve_pad_context(shift, shift_rec_mode_)) {
    case PadUiContext::SHIFT_REC_DEEP:
        handle_shift_rec_snapshot_layer(frame, seq, state, queue);
        break;
    case PadUiContext::SHIFT_SNAPSHOT:
        handle_snapshot_management(frame, seq, state, queue);
        snap_arp_active_ = false;
        break;
    case PadUiContext::NOTE:
        handle_note_pads(frame, seq, state, queue);
        break;
    case PadUiContext::NORMAL:
    default:
        handle_snapshots(frame, seq, state, queue);
        break;
    }

    handle_drums(frame, seq, state, queue);

    // FIX BUG-IR-4: usar frame.now_ms en vez de segunda llamada a to_ms_since_boot()
    if (snap_arp_active_) {
        if (now_ms >= snap_arp_next_ms_) {
            snap_arp_next_ms_ = now_ms + 90;
            uint8_t slot = (snap_arp_step_ & 0x07u);
            snap_arp_step_++;
            if (!state.get_mute_snap(slot)) {
                state.request_trigger(slot);
                enqueue_event(queue, {seq.current_tick(), EVT_PAD_TRIGGER, slot, 1.0f});
            }
        }
    }

    handle_adc(frame, seq, state, queue);

    if (seq.is_step_write_mode() && (just_off & (1u << PAD_SHIFT)) && !shift_used_for_action_)
        seq.on_manual_advance();
    if (just_off & (1u << PAD_SHIFT)) {
        shift_used_for_action_    = false;
        shift_waiting_second_tap_ = false;
    }

    handle_aftertouch(frame, state, queue, seq);
}

// ── handle_transport ───────────────────────────────────────────────

void InputRouter::handle_transport(const ControlFrame& frame, Sequencer& seq,
                                   StateManager& state, bool shift,
                                   RingBuffer<SequencerEvent, 128>& queue)
{
    const uint16_t just_on = frame.pads_just_pressed;

    if (just_on & (1u << PAD_PLAY)) {
        const bool shift_rec_combo = shift && frame.is_pressed(PAD_REC);
        const bool arm_combo       = !shift && frame.is_pressed(PAD_REC);
        const bool seq_edit        = seq.is_step_write_mode() || seq.is_overdub();

        if (shift_rec_combo && seq.is_step_write_mode()) {
            seq.duplicate_previous_step_into_next();
            shift_used_for_action_ = true;
            if (led_ctrl) show_action(ActionFeedback::COPY);
        } else if (shift_rec_combo && seq_edit) {
            shift_used_for_action_ = true;
        } else if (arm_combo) {
            seq.arm_record();
        } else if (shift) {
            note_mode_ = !note_mode_;
            state.set_note_mode(note_mode_);
            if (ui_renderer)
                ui_renderer->show_action_message(note_mode_ ? "NOTE MODE" : "SNAP MODE");
            if (!note_mode_) {
                for (uint8_t logical = 0; logical < NOTE_PAD_COUNT; logical++) {
                    if (note_active_[logical]) {
                        enqueue_event(queue, {seq.current_tick(), EVT_NOTE_OFF,
                                      active_note_midi_[logical], 0.0f});
                        if (midi) midi->send_note_off(active_note_midi_[logical]);
                        note_active_[logical]      = false;
                        active_note_midi_[logical] = 0;
                    }
                }
                state.clear_note_pitch();
            }
            shift_used_for_action_ = true;
            LOG_AUDIO("INPUT: Note Mode %s", note_mode_ ? "ON" : "OFF");
            if (led_ctrl) led_ctrl->on_note_mode_toggle(note_mode_);
        } else {
            if (seq.play_state() == PlayState::STOPPED) seq.play();
            else seq.stop();
        }
    }

    if (just_on & (1u << PAD_REC)) {
        if (!shift && frame.is_pressed(PAD_PLAY)) {
            seq.arm_record();
        } else if (!shift) {
            seq.rec_toggle();
        }
    }
}

// ── handle_snapshot_management ────────────────────────────────────

void InputRouter::handle_snapshot_management(const ControlFrame& frame, Sequencer& seq,
                                             StateManager& state,
                                             RingBuffer<SequencerEvent, 128>&)
{
    (void)seq;
    static constexpr float    kDummyPots[7]   = {0,0,0,0,0,0,0};
    static constexpr uint32_t SNAP_TAP_MAX_MS = 280u;
    const Snapshot* snaps  = state.get_snapshots();
    const uint16_t  just_on  = frame.pads_just_pressed;
    const uint16_t  just_off = frame.pads_just_released;
    const uint32_t  now_ms   = frame.now_ms;

    if (state.is_snapshot_morph_active()) {
        const uint8_t morph_a = state.get_snapshot_morph_a();
        const uint8_t morph_b = state.get_snapshot_morph_b();
        if ((morph_a < NOTE_PAD_COUNT && (just_off & (1u << SNAP_PAD_PHYS[morph_a]))) ||
            (morph_b < NOTE_PAD_COUNT && (just_off & (1u << SNAP_PAD_PHYS[morph_b])))) {
            state.stop_snapshot_morph(state.get_snapshot_morph_amount() >= 0.5f);
            if (morph_a < NOTE_PAD_COUNT) { snap_saved_[morph_a] = true; snap_press_ms_[morph_a] = 0; }
            if (morph_b < NOTE_PAD_COUNT) { snap_saved_[morph_b] = true; snap_press_ms_[morph_b] = 0; }
            shift_used_for_action_ = true;
            return;
        }
    }

    for (uint8_t slot = 0; slot < NOTE_PAD_COUNT; ++slot) {
        const uint8_t pad        = SNAP_PAD_PHYS[slot];
        const bool    slot_valid = snaps[slot].valid;

        if (just_on & (1u << pad)) {
            snap_press_ms_[slot] = now_ms;
            snap_saved_[slot]    = false;
            if (led_ctrl) show_action(ActionFeedback::SAVE_ARM);

            const uint8_t other_slot = find_other_pressed_snapshot_slot(frame, pad);
            if (!state.is_snapshot_morph_active() && other_slot != INVALID_SLOT &&
                other_slot < NOTE_PAD_COUNT &&
                snaps[slot].valid && snaps[other_slot].valid) {
                if (state.start_snapshot_morph(other_slot, slot)) {
                    shift_used_for_action_ = true;
                    snap_saved_[slot]      = true;
                    if (led_ctrl) show_action(ActionFeedback::COPY);
                    continue;
                }
            }
        }

        if (!state.is_snapshot_morph_active() && frame.is_pressed(pad) && !snap_saved_[slot]) {
            const uint32_t held_ms = now_ms - snap_press_ms_[slot];
            if (held_ms >= SNAP_HOLD_ACTION_MS) {
                state.request_save(slot, kDummyPots);
                snap_saved_[slot]      = true;
                shift_used_for_action_ = true;
                if (led_ctrl) show_action(ActionFeedback::SAVE_OK);
            }
        }

        if (just_off & (1u << pad)) {
            const uint32_t held_ms = now_ms - snap_press_ms_[slot];
            if (!state.is_snapshot_morph_active() && !snap_saved_[slot]) {
                if (!slot_valid && held_ms <= SNAP_TAP_MAX_MS) {
                    state.request_save(slot, kDummyPots);
                    if (led_ctrl) show_action(ActionFeedback::SAVE_OK);
                } else if (slot_valid && held_ms <= SNAP_TAP_MAX_MS) {
                    state.set_mute_snap(slot, !state.get_mute_snap(slot));
                    if (led_ctrl) show_action(ActionFeedback::CONDITION);
                }
                shift_used_for_action_ = true;
            }
            snap_saved_[slot]    = false;
            snap_press_ms_[slot] = 0;
        }
    }
}

// ── handle_shift_rec_snapshot_layer ───────────────────────────────

void InputRouter::handle_shift_rec_snapshot_layer(const ControlFrame& frame, Sequencer& seq,
                                                   StateManager& state,
                                                   RingBuffer<SequencerEvent, 128>& queue)
{
    (void)state;
    const uint16_t just_on  = frame.pads_just_pressed;
    const uint16_t just_off = frame.pads_just_released;
    const uint32_t now_ms   = frame.now_ms;

    for (uint8_t slot = 0; slot < 7; ++slot) {
        const uint8_t pad = SNAP_PAD_PHYS[slot];
        const bool    on  = (just_on  & (1u << pad)) != 0;
        const bool    off = (just_off & (1u << pad)) != 0;

        if (on) {
            enqueue_event(queue, {seq.current_tick(), EVT_FX_ON, (uint8_t)(slot + 1), 1.0f});
            seq.record_fx((uint8_t)(slot + 1), true);
            active_punch_fx_ |= (1u << slot);
            if (led_ctrl) led_ctrl->set_punch_fx_mask(active_punch_fx_);
            shift_used_for_action_ = true;
        }
        if (off) {
            enqueue_event(queue, {seq.current_tick(), EVT_FX_OFF, (uint8_t)(slot + 1), 0.0f});
            seq.record_fx((uint8_t)(slot + 1), false);
            active_punch_fx_ &= ~(1u << slot);
            if (led_ctrl) led_ctrl->set_punch_fx_mask(active_punch_fx_);
            shift_used_for_action_ = true;
        }
    }

    const uint8_t arp_pad = SNAP_PAD_PHYS[7];
    const bool arp_on  = (just_on  & (1u << arp_pad)) != 0;
    const bool arp_off = (just_off & (1u << arp_pad)) != 0;

    if (arp_on) {
        snap_arp_active_  = true;
        snap_arp_step_    = 0;
        snap_arp_next_ms_ = now_ms;
        seq.record_arp();
        shift_used_for_action_ = true;
    }
    if (arp_off) {
        snap_arp_active_ = false;
    }
}

// ── handle_snapshots ───────────────────────────────────────────────

void InputRouter::handle_snapshots(const ControlFrame& frame, Sequencer& seq,
                                   StateManager& state,
                                   RingBuffer<SequencerEvent, 128>& queue)
{
    static constexpr uint32_t SNAP_VOICE_HOLD_MS          = 120u;
    static constexpr float    SNAP_DRONE_RELEASE_THRESHOLD = 0.97f;

    // FIX BUG-IR-3: usar frame.pads_just_released y frame.now_ms
    // en vez de llamar de nuevo a pads.get_just_released() / to_ms_since_boot()
    const uint16_t just_on  = frame.pads_just_pressed;
    const uint16_t just_off = frame.pads_just_released;
    const uint32_t now_ms   = frame.now_ms;

    auto quantize_gate_steps = [](uint32_t press_tick, uint32_t release_tick) -> uint8_t {
        const uint32_t held_ticks  = (release_tick > press_tick) ? (release_tick - press_tick) : 0u;
        const uint32_t step_ticks  = Sequencer::INT_PPQN / 4u;
        uint32_t steps = (held_ticks + (step_ticks / 2u)) / step_ticks;
        if (steps < 1u) steps = 1u;
        if (steps > 8u) steps = 8u;
        return static_cast<uint8_t>(steps);
    };

    for (uint8_t slot = 0; slot < NOTE_PAD_COUNT; ++slot) {
        const uint8_t pad             = SNAP_PAD_PHYS[slot];
        const bool    newly_pressed   = (just_on  & (1u << pad)) != 0;
        const bool    newly_released  = (just_off & (1u << pad)) != 0;
        const bool    pressed         = frame.is_pressed(pad);

        if (newly_pressed) {
            snap_press_ms_[slot]     = now_ms;
            snap_press_tick_[slot]   = seq.current_tick();
            snap_peak_pressure_[slot]= frame.pressure(pad);
            snap_gesture_[slot]      = SnapshotGestureState::TAP_PENDING;
            if (ui_renderer) ui_renderer->show_action_message("SNAP");
        }

        if (pressed && snap_gesture_[slot] != SnapshotGestureState::IDLE) {
            const float p = frame.pressure(pad);
            if (p > snap_peak_pressure_[slot]) snap_peak_pressure_[slot] = p;
        }

        if (pressed && snap_gesture_[slot] == SnapshotGestureState::TAP_PENDING) {
            const uint32_t held_ms = now_ms - snap_press_ms_[slot];
            if (held_ms >= SNAP_VOICE_HOLD_MS) {
                const float vel = 0.65f + snap_peak_pressure_[slot] * 0.35f;
                enqueue_event(queue, {seq.current_tick(), EVT_SNAPSHOT_VOICE_ON, slot, vel});
                snap_gesture_[slot] = SnapshotGestureState::VOICE_HELD;
                if (ui_renderer) ui_renderer->show_action_message("VOICE");
            }
        }

        if (newly_released) {
            if (snap_gesture_[slot] == SnapshotGestureState::TAP_PENDING) {
                state.request_trigger(slot);
                enqueue_event(queue, {seq.current_tick(), EVT_PAD_TRIGGER, slot, 1.0f});
                seq.record_snapshot(slot);
                if (led_ctrl) led_ctrl->on_snapshot_trigger(slot);
            } else if (snap_gesture_[slot] == SnapshotGestureState::VOICE_HELD) {
                const bool    latch      = state.get_env_loop() ||
                                           state.get_env_release() > SNAP_DRONE_RELEASE_THRESHOLD;
                const float   vel        = 0.65f + snap_peak_pressure_[slot] * 0.35f;
                const uint8_t gate_steps = quantize_gate_steps(snap_press_tick_[slot],
                                                                seq.current_tick());
                seq.record_snapshot_voice(slot, vel, latch, gate_steps, snap_press_tick_[slot]);
                enqueue_event(queue, {seq.current_tick(), EVT_SNAPSHOT_VOICE_OFF, slot, 0.0f});
                if (ui_renderer)
                    ui_renderer->show_action_message(latch ? "DRONE" : "VOICE");
            }

            snap_gesture_[slot]       = SnapshotGestureState::IDLE;
            snap_press_ms_[slot]      = 0u;
            snap_press_tick_[slot]    = 0u;
            snap_peak_pressure_[slot] = 0.0f;
        }
    }
}

// ── handle_note_pads ───────────────────────────────────────────────

void InputRouter::handle_note_pads(const ControlFrame& frame, Sequencer& seq,
                                   StateManager& state,
                                   RingBuffer<SequencerEvent, 128>& queue)
{
    ScaleId scale = (ScaleId)state.get_scale_id();
    uint8_t root  = state.get_root();

    for (uint8_t logical = 0; logical < NOTE_PAD_COUNT; logical++) {
        const uint8_t pad      = SNAP_PAD_PHYS[logical];
        const bool    pressed  = frame.just_pressed(pad);
        const bool    released = frame.just_released(pad);

        if (pressed) {
            const uint8_t note  = NoteMode::pad_to_midi(logical, scale, root);
            const float   vel   = 0.75f + frame.pressure(pad) * 0.25f;
            const float   ratio = NoteMode::midi_to_pitch_ratio(note);

            active_note_midi_[logical] = note;
            note_active_[logical]      = true;

            state.set_note_pitch(ratio);
            state.set_active_note(logical, note);
            enqueue_event(queue, {seq.current_tick(), EVT_NOTE_ON, note, vel});
            seq.record_note(logical, note, vel);
            if (midi) midi->send_note_on(note, (uint8_t)(vel * 127.0f));

            LOG_AUDIO("NOTE ON pad=%u logical=%u note=%u(%s) ratio=%.3f",
                      pad, logical, note, NoteMode::midi_note_name(note), (double)ratio);
        }

        if (released && note_active_[logical]) {
            const uint8_t note = active_note_midi_[logical];
            note_active_[logical] = false;

            bool any_active = false;
            for (uint8_t p = 0; p < NOTE_PAD_COUNT; p++)
                if (note_active_[p]) { any_active = true; break; }
            if (!any_active) {
                state.clear_note_pitch();
                state.clear_active_note();
            }

            enqueue_event(queue, {seq.current_tick(), EVT_NOTE_OFF, note, 0.0f});
            if (midi) midi->send_note_off(note);
        }
    }
}

// ── handle_drums ───────────────────────────────────────────────────

void InputRouter::handle_drums(const ControlFrame& frame, Sequencer& seq,
                               StateManager& state,
                               RingBuffer<SequencerEvent, 128>& queue)
{
    struct DrumMap { uint8_t pad; DrumId id; };
    static constexpr DrumMap drums[] = {
        {PAD_KICK,  DRUM_KICK},
        {PAD_SNARE, DRUM_SNARE},
        {PAD_HAT,   DRUM_HAT}
    };

    const bool shift        = frame.is_pressed(PAD_SHIFT);
    const bool rec          = frame.is_pressed(PAD_REC);
    const bool random_combo = shift && frame.is_pressed(PAD_KICK) && frame.is_pressed(PAD_SNARE);
    const bool mutate_combo = shift && frame.is_pressed(PAD_KICK) &&
                              !frame.is_pressed(PAD_SNARE) && !rec;
    const bool gesture_fx_layer = shift && rec;
    const bool mute_held        = frame.is_pressed(PAD_MUTE) && !shift;
    const bool clear_drum_layer = shift &&
                                  (seq.is_step_write_mode() || seq.is_overdub()) &&
                                  !gesture_fx_layer && !random_combo && !mutate_combo &&
                                  !mute_held && !frame.is_pressed(PAD_PLAY) && !rec;

    // FIX BUG-IR-5: usar frame.now_ms en vez de segunda llamada a to_ms_since_boot()
    const uint32_t now_ms = frame.now_ms;

    for (uint8_t i = 0; i < 3; i++) {
        const uint8_t pad     = drums[i].pad;
        const DrumId  id      = drums[i].id;
        const bool    just_on  = frame.just_pressed(pad);
        const bool    just_off = frame.just_released(pad);
        const bool    pressed  = frame.is_pressed(pad);

        if (clear_drum_layer && just_on) {
            seq.clear_current_drum_layer();
            shift_used_for_action_ = true;
            continue;
        }
        if (mute_held && just_on) {
            state.toggle_mute_drum(i);
            drum_rolling_[i] = false;
            continue;
        }
        if (gesture_fx_layer) {
            FxId fx = FX_BEAT_REPEAT;
            switch (pad) {
                case PAD_KICK:  fx = FX_BEAT_REPEAT; break;
                case PAD_SNARE: fx = FX_VIBRATO;     break;
                case PAD_HAT:   fx = FX_FREEZE;      break;
                default: break;
            }
            if (just_on) {
                enqueue_event(queue, {seq.current_tick(), EVT_FX_ON, (uint8_t)fx, 1.0f});
                seq.record_fx((uint8_t)fx, true);
                shift_used_for_action_ = true;
            }
            if (just_off) {
                enqueue_event(queue, {seq.current_tick(), EVT_FX_OFF, (uint8_t)fx, 0.0f});
                seq.record_fx((uint8_t)fx, false);
                shift_used_for_action_ = true;
            }
            drum_rolling_[i] = false;
            continue;
        }
        if ((random_combo && (pad == PAD_KICK || pad == PAD_SNARE)) ||
            (mutate_combo &&  pad == PAD_KICK)) {
            drum_rolling_[i] = false;
            continue;
        }

        if (just_on) {
            drum_press_ms_[i] = now_ms;
            drum_rolling_[i]  = false;
        }
        if (pressed && !drum_rolling_[i]) {
            if (now_ms - drum_press_ms_[i] >= ROLL_THRESHOLD_MS) {
                drum_rolling_[i] = true;
                if (!state.get_mute_drum(i)) {
                    enqueue_event(queue, {seq.current_tick(), EVT_DRUM_HIT, (uint8_t)id, 1.0f});
                    seq.record_drum(id);
                    enqueue_event(queue, {seq.current_tick(), EVT_ROLL_ON,  (uint8_t)id, 1.0f});
                }
            }
        }
        if (just_off) {
            if (drum_rolling_[i]) {
                drum_rolling_[i] = false;
                if (!state.get_mute_drum(i))
                    enqueue_event(queue, {seq.current_tick(), EVT_ROLL_OFF, (uint8_t)id, 0.0f});
            } else {
                if (!state.get_mute_drum(i)) {
                    enqueue_event(queue, {seq.current_tick(), EVT_DRUM_HIT, (uint8_t)id, 1.0f});
                    seq.record_drum(id);
                }
            }
        }
    }
}

// ── resolve_pot_param ──────────────────────────────────────────────
//
// V1.22: mapa RESTAURADO al código original de V1.21 (bytebeat engine params).
//
// El comentario interno decía MACRO/TONAL/DRIVE... pero era documentación
// desactualizada de V1.18. Los STAGE notes de Stage10Z y Stage11 indican
// explícitamente que "io/input_router.cpp is the authoritative live control map",
// es decir el código es la fuente de verdad, no el comentario.
//
// Mapa real (el que tenía el firmware funcionando en V1.21):
//        P0          P1      P2      P3        P4          P5            P6
// NORM:  FORMULA_A   RATE    SHIFT   MASK      FEEDBACK    FILTER_MACRO  MORPH
// SHIFT: FORMULA_B   JITTER  PHASE   XOR_FOLD  BB_SEED     RESONANCE     ENV_MACRO
// SH+RC: REVERB_ROOM REVWET  CHORUS  DRUM_DEC  DRUM_COLOR  DUCK_AMOUNT   DELAY_WET
//
// La capa SHIFT+REC era la única con parámetros de performance/FX globales.
// El morph especial de snapshot (pot 6 en SHIFT) se mantiene coherente
// porque kShiftMap[6] = PARAM_ENV_MACRO, y el intercept del morph activo
// tiene su propia condición explícita (ver handle_adc).

bool InputRouter::is_drum_bus_param(ParamId id) const {
    return id == PARAM_DRUM_DECAY || id == PARAM_DRUM_COLOR || id == PARAM_DUCK_AMOUNT;
}

ParamId InputRouter::resolve_pot_param(uint8_t pot_index, bool shift, bool shift_rec) const {
    // Mapa V1.21 original — bytebeat engine params en capas NORM y SHIFT
    static constexpr ParamId kBaseMap[7] = {
        PARAM_FORMULA_A,    // P0
        PARAM_RATE,         // P1
        PARAM_SHIFT,        // P2
        PARAM_MASK,         // P3
        PARAM_FEEDBACK,     // P4
        PARAM_FILTER_MACRO, // P5
        PARAM_MORPH,        // P6
    };
    static constexpr ParamId kShiftMap[7] = {
        PARAM_FORMULA_B,  // P0
        PARAM_JITTER,     // P1
        PARAM_PHASE,      // P2
        PARAM_XOR_FOLD,   // P3
        PARAM_BB_SEED,    // P4
        PARAM_RESONANCE,  // P5
        PARAM_ENV_MACRO,  // P6
    };
    static constexpr ParamId kShiftRecMap[7] = {
        PARAM_REVERB_ROOM,  // P0
        PARAM_REVERB_WET,   // P1
        PARAM_CHORUS,       // P2
        PARAM_DRUM_DECAY,   // P3
        PARAM_DRUM_COLOR,   // P4
        PARAM_DUCK_AMOUNT,  // P5
        PARAM_DELAY_WET,    // P6
    };

    if (pot_index >= 7) return PARAM_MACRO;
    if (shift_rec) return kShiftRecMap[pot_index];
    if (shift)     return kShiftMap[pot_index];
    return kBaseMap[pot_index];
}

// ── handle_adc ─────────────────────────────────────────────────────

void InputRouter::handle_adc(const ControlFrame& frame, Sequencer& seq,
                              StateManager& state,
                              RingBuffer<SequencerEvent, 128>& queue)
{
    // FIX BUG-IR-2: usar shift del frame actual, no last_shift_ (stale)
    const bool shift     = frame.is_pressed(PAD_SHIFT);
    const bool shift_rec = shift && shift_rec_mode_;
    const bool clear_param_mode = shift && !shift_rec &&
                                  (seq.is_step_write_mode() || seq.is_overdub());

    const uint32_t snapshot_epoch = state.get_snapshot_epoch();
    if (snapshot_epoch != last_snapshot_epoch_) {
        for (uint8_t i = 0; i < AdcHandler::NUM_POTS; i++) {
            pot_state_[i] = PotState::CATCHING;
            const ParamId catch_param = resolve_pot_param(i, shift, shift_rec);
            pot_catch_val_[i] = (state.is_snapshot_morph_active() && shift && !shift_rec
                                 && catch_param == PARAM_MORPH)
                              ? state.get_snapshot_morph_amount()
                              : state.get_param_normalized(catch_param);
        }
        last_snapshot_epoch_ = snapshot_epoch;
        last_layer_          = 0xFFu;
    }

    // Soft takeover: detectar cambio de capa
    const uint8_t cur_layer = shift_rec ? 2u : (shift ? 1u : 0u);
    if (cur_layer != last_layer_) {
        for (uint8_t i = 0; i < AdcHandler::NUM_POTS; i++) {
            pot_state_[i] = PotState::CATCHING;
            const ParamId catch_param = resolve_pot_param(i, shift, shift_rec);
            pot_catch_val_[i] = (state.is_snapshot_morph_active() && shift && !shift_rec
                                 && catch_param == PARAM_MORPH)
                              ? state.get_snapshot_morph_amount()
                              : state.get_param_normalized(catch_param);
        }
        last_layer_ = cur_layer;
        if (led_ctrl) led_ctrl->on_layer_change(cur_layer);
    }

    for (uint8_t i = 0; i < AdcHandler::NUM_POTS; i++) {
        // FIX: usar frame.pots[] en vez de adc.get(i)
        float val = frame.pot(i);

        // Soft takeover
        if (pot_state_[i] == PotState::CATCHING) {
            const float catch_v = pot_catch_val_[i];
            const bool  crossed = (last_pot_[i] <= catch_v && val >= catch_v) ||
                                  (last_pot_[i] >= catch_v && val <= catch_v) ||
                                  (fabsf(val - catch_v) < 0.015f);
            if (crossed) {
                pot_state_[i] = PotState::TRACKING;
                last_pot_[i]  = val;
            } else {
                last_pot_[i] = val;
                continue;
            }
        }

        if (fabsf(val - last_pot_[i]) <= 0.01f) continue;

        // Snapshot morph activo: pot 6 en SHIFT controlaba PARAM_MORPH (kBaseMap[6]).
        // Detectamos por param identity en vez de índice para ser robustos
        // frente a cambios de mapa futuros.
        const ParamId param_check = resolve_pot_param(i, shift, shift_rec);
        if (state.is_snapshot_morph_active() && shift && !shift_rec &&
            param_check == PARAM_MORPH) {
            state.update_snapshot_morph(val);
            if (ui_renderer) ui_renderer->show_parameter(PARAM_MORPH, val, "SNAP");
            last_pot_[i]           = val;
            shift_used_for_action_ = true;
            continue;
        }

        const ParamId param       = param_check;
        EventType     event_type  = EVT_PARAM_CHANGE;
        uint8_t       event_target = static_cast<uint8_t>(param);

        if (clear_param_mode && frame.is_pressed(PAD_MUTE)) {
            seq.clear_current_param_lock(param);
            shift_used_for_action_ = true;
            last_pot_[i]           = val;
            continue;
        }

        if (shift_rec) {
            state.set_bus_param(param, val);
            if (is_drum_bus_param(param)) {
                event_type = EVT_DRUM_PARAM;
                switch (param) {
                case PARAM_DRUM_DECAY:  event_target = DRUM_PARAM_DECAY; break;
                case PARAM_DRUM_COLOR:  event_target = DRUM_PARAM_COLOR; break;
                case PARAM_DUCK_AMOUNT: event_target = DRUM_PARAM_DUCK;  break;
                default: break;
                }
            }
        } else {
            state.set_patch_param(param, val);
        }

        enqueue_event(queue, {seq.current_tick(), event_type, event_target, val});
        seq.record_param(param, val);
        if (shift) shift_used_for_action_ = true;

        if (param == PARAM_BEAT_REPEAT_DIV && led_ctrl)
            led_ctrl->on_beat_repeat_div(val);
        if (ui_renderer)
            ui_renderer->show_parameter(param, val);

        last_pot_[i] = val;
    }
}

// ── handle_encoder ─────────────────────────────────────────────────

void InputRouter::handle_encoder(const ControlFrame& frame, Sequencer& seq,
                                 StateManager& state)
{
    if (!encoder_ready_) {
        encoder_.init(ENC_A_PIN, ENC_B_PIN, ENC_SW_PIN);
        encoder_ready_ = true;
    }
    encoder_.update();

    const bool        shift_held = frame.is_pressed(PAD_SHIFT);
    const bool        rec_held   = frame.is_pressed(PAD_REC);
    const EncoderMode mode_now   = state.get_encoder_mode();
    const bool        seq_edit   = seq.is_step_write_mode() || seq.is_overdub();
    const uint32_t    now_ms     = frame.now_ms;

    // HOME: hold del encoder SW
    if (encoder_.is_pressed()) {
        if (!encoder_hold_started_) {
            encoder_hold_start_ms_ = now_ms;
            encoder_hold_started_  = true;
            home_full_fired_       = false;
        }
        const uint32_t held_ms  = now_ms - encoder_hold_start_ms_;
        const float    progress  = (float)held_ms / (float)HOME_FULL_MS;
        const uint8_t  vis_level = (held_ms >= HOME_FULL_MS) ? 2u
                                 : (held_ms >= 700u)         ? 1u : 0u;
        if (led_ctrl) led_ctrl->on_home_progress(progress > 1.0f ? 1.0f : progress, vis_level);
    } else {
        encoder_hold_started_ = false;
    }

    if (encoder_.read_hold()) {
        if (!home_full_fired_) {
            state.home_reset(StateManager::HomeLevel::SOFT);
            seq.set_bpm((float)state.get_encoder_state().bpm);
            for (uint8_t pi = 0; pi < AdcHandler::NUM_POTS; pi++) {
                pot_state_[pi]     = PotState::CATCHING;
                pot_catch_val_[pi] = state.get_param_normalized(
                    resolve_pot_param(pi, shift_held, shift_held && shift_rec_mode_));
            }
            last_layer_ = 0xFF;
            if (led_ctrl) {
                led_ctrl->on_home_progress(0.0f, 0);
                led_ctrl->show_encoder_state(state.get_encoder_state());
            }
        }
        home_full_fired_ = false;
    }

    if (encoder_hold_started_ && !home_full_fired_) {
        const uint32_t held_ms = now_ms - encoder_hold_start_ms_;
        if (held_ms >= HOME_FULL_MS) {
            state.home_reset(StateManager::HomeLevel::FULL);
            seq.set_bpm((float)state.get_encoder_state().bpm);
            home_full_fired_ = true;
            for (uint8_t pi = 0; pi < AdcHandler::NUM_POTS; pi++) {
                pot_state_[pi]     = PotState::CATCHING;
                pot_catch_val_[pi] = state.get_param_normalized(
                    resolve_pot_param(pi, shift_held, shift_held && shift_rec_mode_));
            }
            last_layer_      = 0xFF;
            active_punch_fx_ = 0;
            if (led_ctrl) {
                led_ctrl->set_punch_fx_mask(0);
                led_ctrl->on_home_progress(0.0f, 0);
                led_ctrl->show_encoder_state(state.get_encoder_state());
            }
        }
    }

    if (encoder_.read_click()) {
        if (seq_edit && shift_held && !rec_held && mode_now == EncoderMode::SPACE) {
            seq.toggle_current_step_fill();
            shift_used_for_action_ = true;
            show_action(ActionFeedback::STEP_FILL);
            if (led_ctrl) led_ctrl->show_fill_overlay(seq.current_step_fill());
        } else if (seq_edit && shift_held && rec_held && mode_now == EncoderMode::DENSITY) {
            seq.reset_current_step_condition();
            shift_used_for_action_ = true;
            show_action(ActionFeedback::CONDITION);
            show_condition_overlay(seq);
        } else if (seq_edit && shift_held && rec_held && mode_now == EncoderMode::SWING) {
            seq.reset_current_step_microtiming();
            if (led_ctrl) led_ctrl->show_microtiming_overlay(seq.current_step_microtiming());
        } else if (seq_edit && shift_held && rec_held && mode_now == EncoderMode::MUTATE) {
            seq.reset_current_step_ratchet();
            if (led_ctrl) led_ctrl->show_ratchet_overlay(seq.current_step_ratchet());
        } else if (seq_edit && shift_held && !rec_held && mode_now == EncoderMode::MUTATE) {
            seq.reset_current_step_chance();
            if (led_ctrl) {
                EncoderState overlay        = state.get_encoder_state();
                overlay.mode                = EncoderMode::MUTATE;
                overlay.mutate_amount       = seq.current_step_chance();
                led_ctrl->show_encoder_state(overlay);
            }
        } else if (!seq_edit && state.is_note_mode() && shift_held && !rec_held &&
                   mode_now == EncoderMode::SCALE) {
            state.cycle_note_voice_source();
            shift_used_for_action_ = true;
            if (ui_renderer) ui_renderer->show_action_message(state.note_voice_source_name());
        } else if (!seq_edit && mode_now == EncoderMode::SWING && shift_held && rec_held &&
                   seq.groove_template() == GrooveTemplate::IDM) {
            seq.set_idm_variant(IDMVariant::TIGHT);
            if (led_ctrl) show_idm_variant_overlay(seq);
        } else if (mode_now == EncoderMode::MUTATE && rec_held) {
            const float amount = state.get_mutate_amount();
            state.mutate_active_snapshot(amount, shift_held);
            seq.mutate_generative_sequence(amount, shift_held);
        } else {
            state.next_encoder_mode();
            if (led_ctrl) led_ctrl->show_encoder_state(state.get_encoder_state());
        }
    }

    int delta = encoder_.read_delta();
    if (delta != 0) {
        if (seq_edit && shift_held && rec_held &&
            state.get_encoder_mode() == EncoderMode::DENSITY) {
            seq.adjust_current_step_condition(delta);
            shift_used_for_action_ = true;
            show_action(ActionFeedback::CONDITION);
            show_condition_overlay(seq);
        } else if (seq_edit && shift_held && rec_held &&
                   state.get_encoder_mode() == EncoderMode::SWING) {
            seq.adjust_current_step_microtiming(delta);
            shift_used_for_action_ = true;
            if (led_ctrl) led_ctrl->show_microtiming_overlay(seq.current_step_microtiming());
        } else if (seq_edit && shift_held && rec_held &&
                   state.get_encoder_mode() == EncoderMode::MUTATE) {
            seq.adjust_current_step_ratchet(delta);
            shift_used_for_action_ = true;
            if (led_ctrl) led_ctrl->show_ratchet_overlay(seq.current_step_ratchet());
        } else if (seq_edit && shift_held && !rec_held &&
                   state.get_encoder_mode() == EncoderMode::MUTATE) {
            seq.adjust_current_step_chance(delta);
            shift_used_for_action_ = true;
            if (led_ctrl) {
                EncoderState overlay  = state.get_encoder_state();
                overlay.mode          = EncoderMode::MUTATE;
                overlay.mutate_amount = seq.current_step_chance();
                led_ctrl->show_encoder_state(overlay);
            }
        } else if (state.get_encoder_mode() == EncoderMode::SWING && shift_held && !rec_held) {
            seq.adjust_groove_template(delta);
            shift_used_for_action_ = true;
            if (led_ctrl) show_groove_overlay(seq, state);
        } else if (!seq_edit && shift_held && rec_held &&
                   state.get_encoder_mode() == EncoderMode::SWING &&
                   seq.groove_template() == GrooveTemplate::IDM) {
            seq.adjust_idm_variant(delta);
            shift_used_for_action_ = true;
            if (led_ctrl) show_idm_variant_overlay(seq);
        } else if (!seq_edit && shift_held && rec_held &&
                   state.get_encoder_mode() == EncoderMode::BPM) {
            seq.adjust_play_direction(delta);
            shift_used_for_action_ = true;
            if (led_ctrl) show_play_direction_overlay(seq);
        } else {
            state.encoder_delta(delta, shift_held);

            const EncoderMode   mode_after = state.get_encoder_mode();
            const EncoderState& enc        = state.get_encoder_state();
            if      (mode_after == EncoderMode::BPM)     seq.set_bpm((float)enc.bpm);
            else if (mode_after == EncoderMode::SWING)    seq.set_swing(enc.swing_amount);
            else if (mode_after == EncoderMode::DENSITY)  seq.set_density(enc.density_amount);
            else if (mode_after == EncoderMode::CHAOS)    seq.set_chaos(enc.chaos_amount);
            else if (mode_after == EncoderMode::SPACE)    state.set_space_macro(enc.space_amount);

            if (led_ctrl) led_ctrl->show_encoder_state(enc);
        }
    }
}

// ── handle_aftertouch ──────────────────────────────────────────────

void InputRouter::handle_aftertouch(const ControlFrame& frame, StateManager& state,
                                    RingBuffer<SequencerEvent, 128>& queue,
                                    Sequencer& seq)
{
    static constexpr float DEADZONE  = 0.02f;
    static constexpr float HYST_EMIT = 0.015f;

    for (uint8_t phys = 0; phys < CapPadHandler::NUM_PADS; phys++) {
        float p = frame.pressure(phys);
        if (fabsf(p - last_pressure_[phys]) < HYST_EMIT) continue;
        last_pressure_[phys] = p;
        if (p < DEADZONE) p = 0.0f;

        const uint8_t logical = SNAP_SLOT_FROM_PHYS[phys];

        // Prioridad 1: Note Mode
        if (note_mode_ && logical != INVALID_SLOT && note_active_[logical]) {
            enqueue_event(queue, {seq.current_tick(), EVT_AFTERTOUCH,
                          active_note_midi_[logical], p});
            continue;
        }

        // Prioridad 2: PAD_MUTE → grain freeze wet
        if (phys == PAD_MUTE) {
            const bool shift           = frame.is_pressed(PAD_SHIFT);
            const bool rec             = frame.is_pressed(PAD_REC);
            const bool transport_combo = shift || rec || frame.is_pressed(PAD_PLAY);
            const bool pad_combo       = any_snapshot_pad_pressed(frame) ||
                                         any_drum_pad_pressed(frame);
            const bool seq_edit        = seq.is_step_write_mode() || seq.is_overdub();
            if (!transport_combo && !pad_combo && !seq_edit)
                enqueue_event(queue, {seq.current_tick(), EVT_AFTERTOUCH, PAD_MUTE, p});
            continue;
        }

        // Prioridad 3: SNAP pads → reverb wet momentáneo
        if (logical != INVALID_SLOT) {
            enqueue_event(queue, {seq.current_tick(), EVT_AFTERTOUCH,
                          AT_TARGET_REVERB_SNAP, p});
            continue;
        }

        (void)state;
    }
}
