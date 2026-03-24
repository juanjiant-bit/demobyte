#pragma once
// input_router.h — BYT3/YUY0 V1.22
//
// Cambios V1.22:
//   - process() y todos los handlers reciben const ControlFrame& en vez de
//     CapPadHandler& + AdcHandler&
//   - find_other_pressed_snapshot_slot() pasa a método privado del router
//     operando sobre ControlFrame
//   - BUG FIX: kBaseMap / kShiftMap alineados con el comentario de la UI
//     (MACRO/TONAL/DRIVE/ENV_ATTACK/ENV_RELEASE/GLIDE/DELAY_DIV)
//   - BUG FIX: handle_adc ya no usa last_shift_ (stale) sino el shift
//     derivado del frame actual

#include "control_frame.h"
#include "encoder.h"
#include "../sequencer/sequencer.h"
#include "../state/state_manager.h"
#include "../utils/ring_buffer.h"
#include "../sequencer/event_types.h"
#include "../midi/uart_midi.h"
#include "../synth/note_mode.h"
#include "../led/led_controller.h"
#include "../ui/ui_renderer.h"

// Pads físicos fijos del layout 3x5
//         COL0      COL1      COL2      COL3      COL4
// ROW0  [ REC ][ PLAY ][ SHIFT ][  C  ][  F  ]
// ROW1  [MUTE ][ HAT  ][  A   ][  D  ][  G  ]
// ROW2  [KICK ][SNARE ][  B   ][  E  ][  H  ]
static constexpr uint8_t PAD_REC   =  0;
static constexpr uint8_t PAD_PLAY  =  1;
static constexpr uint8_t PAD_SHIFT =  2;
static constexpr uint8_t PAD_MUTE  =  5;
static constexpr uint8_t PAD_HAT   =  6;
static constexpr uint8_t PAD_KICK  = 10;
static constexpr uint8_t PAD_SNARE = 11;

// Pads físicos donde viven snapshots / notas
//   slot0=C, slot1=F, slot2=A, slot3=D,
//   slot4=G, slot5=B, slot6=E, slot7=H
static constexpr uint8_t SNAP_PAD_PHYS[8] = {3, 4, 7, 8, 9, 12, 13, 14};

// Mapa físico → slot lógico (0xFF = no es snapshot pad)
static constexpr uint8_t SNAP_SLOT_FROM_PHYS[CapPadHandler::NUM_PADS] = {
    0xFF, 0xFF, 0xFF, 0, 1,
    0xFF, 0xFF, 2, 3, 4,
    0xFF, 0xFF, 5, 6, 7
};

static constexpr uint8_t NOTE_PAD_COUNT = 8;
static constexpr uint8_t INVALID_SLOT   = 0xFF;

enum class PadUiContext : uint8_t {
    NORMAL = 0,
    SHIFT_SNAPSHOT,
    SHIFT_REC_DEEP,
    NOTE
};

enum class SnapshotGestureState : uint8_t {
    IDLE = 0,
    TAP_PENDING,
    VOICE_HELD,
};

enum class AftertouchMode : uint8_t {
    NONE          = 0,
    STUTTER_DEPTH = 1,
};

class LedController;

class InputRouter {
public:
    AftertouchMode aftertouch_mode = AftertouchMode::STUTTER_DEPTH;
    UartMidi*      midi       = nullptr;
    LedController* led_ctrl   = nullptr;
    UiRenderer*    ui_renderer = nullptr;

    // V1.22: firma principal — recibe ControlFrame en vez de hardware directo
    void process(const ControlFrame& frame,
                 Sequencer& seq, StateManager& state,
                 RingBuffer<SequencerEvent, 128>& queue);

    bool is_shift_rec_active() const { return shift_rec_mode_; }

private:
    PadUiContext resolve_pad_context(bool shift, bool shift_rec) const;

    void handle_transport (const ControlFrame& frame, Sequencer& seq,
                           StateManager& state, bool shift,
                           RingBuffer<SequencerEvent, 128>& queue);
    void handle_snapshot_management(const ControlFrame& frame, Sequencer& seq,
                           StateManager& state,
                           RingBuffer<SequencerEvent, 128>& queue);
    void handle_shift_rec_snapshot_layer(const ControlFrame& frame, Sequencer& seq,
                           StateManager& state,
                           RingBuffer<SequencerEvent, 128>& queue);
    void handle_snapshots (const ControlFrame& frame, Sequencer& seq,
                           StateManager& state,
                           RingBuffer<SequencerEvent, 128>& queue);
    void handle_note_pads (const ControlFrame& frame, Sequencer& seq,
                           StateManager& state,
                           RingBuffer<SequencerEvent, 128>& queue);
    void handle_drums     (const ControlFrame& frame, Sequencer& seq,
                           StateManager& state,
                           RingBuffer<SequencerEvent, 128>& queue);
    void handle_encoder   (const ControlFrame& frame, Sequencer& seq,
                           StateManager& state);
    void handle_adc       (const ControlFrame& frame, Sequencer& seq,
                           StateManager& state,
                           RingBuffer<SequencerEvent, 128>& queue);
    void handle_aftertouch(const ControlFrame& frame, StateManager& state,
                           RingBuffer<SequencerEvent, 128>& queue,
                           Sequencer& seq);

    ParamId resolve_pot_param(uint8_t pot_index, bool shift, bool shift_rec) const;
    bool    is_drum_bus_param(ParamId id) const;

    bool handle_random_combo(const ControlFrame& frame, Sequencer& seq,
                             StateManager& state, bool shift);
    bool handle_mutate_combo(const ControlFrame& frame, Sequencer& seq,
                             StateManager& state);

    bool enqueue_event(RingBuffer<SequencerEvent, 128>& queue, const SequencerEvent& ev);

    bool any_snapshot_pad_pressed(const ControlFrame& frame) const;
    bool any_drum_pad_pressed    (const ControlFrame& frame) const;

    // V1.22: migrado de función libre a método privado (opera sobre frame)
    uint8_t find_other_pressed_snapshot_slot(const ControlFrame& frame,
                                             uint8_t exclude_pad) const;

    void show_action(ActionFeedback action);
    void show_condition_overlay(Sequencer& seq);
    void show_groove_overlay(Sequencer& seq, StateManager& state);
    void show_idm_variant_overlay(Sequencer& seq);
    void show_play_direction_overlay(Sequencer& seq);

    // ── Estado persistente ────────────────────────────────────────
    float    last_pot_          [7]                       = {};
    uint32_t drum_press_ms_     [3]                       = {};
    bool     drum_rolling_      [3]                       = {};
    float    last_pressure_     [CapPadHandler::NUM_PADS] = {};
    bool     last_shift_                                  = false;
    uint32_t snap_press_ms_     [8] = {};
    uint32_t snap_press_tick_   [8] = {};
    float    snap_peak_pressure_[8] = {};
    SnapshotGestureState snap_gesture_[8] = {};
    bool     snap_saved_        [8] = {};
    static constexpr uint32_t SNAP_HOLD_ACTION_MS = 650u;

    bool     note_mode_         = false;
    uint8_t  active_note_midi_  [NOTE_PAD_COUNT] = {};
    bool     note_active_       [NOTE_PAD_COUNT] = {};
    bool     shift_rec_mode_    = false;
    bool     random_combo_active_ = false;
    bool     mutate_combo_active_ = false;
    bool     mutate_wild_fired_   = false;
    uint32_t mutate_combo_ms_     = 0;
    Encoder  encoder_;
    bool     encoder_ready_       = false;
    bool     random_wild_fired_   = false;
    uint32_t random_combo_ms_     = 0;
    uint32_t dropped_queue_events_= 0;
    bool     snap_arp_active_     = false;
    uint8_t  snap_arp_step_       = 0;
    uint32_t snap_arp_next_ms_    = 0;
    bool     shift_used_for_action_      = false;
    uint32_t last_shift_tap_ms_          = 0;
    bool     shift_waiting_second_tap_   = false;

    // Soft takeover
    enum class PotState : uint8_t { TRACKING = 0, CATCHING };
    PotState pot_state_     [7] = {};
    float    pot_catch_val_ [7] = {};
    uint8_t  last_layer_        = 0;
    uint32_t last_snapshot_epoch_ = 0;

    // HOME via hold del encoder
    bool     home_full_fired_       = false;
    bool     encoder_hold_started_  = false;
    uint32_t encoder_hold_start_ms_ = 0;
    static constexpr uint32_t HOME_FULL_MS = 1500u;

    // Punch FX activos
    uint8_t  active_punch_fx_ = 0;

    static constexpr uint32_t RANDOM_GUARD_MS = 400u;
    static constexpr uint32_t MUTATE_HOLD_MS  = 450u;
};
