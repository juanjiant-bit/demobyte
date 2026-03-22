#pragma once
// event_types.h — Bytebeat Machine V1.21
#include <cstdint>

enum EventType : uint8_t {
    EVT_PAD_TRIGGER,
    EVT_PARAM_CHANGE,
    EVT_DRUM_HIT,
    EVT_ROLL_ON,
    EVT_ROLL_OFF,
    EVT_FX_ON,
    EVT_FX_OFF,
    EVT_MUTATE,
    EVT_AFTERTOUCH,
    EVT_CLOCK_TICK,
    EVT_NOTE_ON,
    EVT_NOTE_OFF,
    EVT_SNAPSHOT_VOICE_ON,
    EVT_SNAPSHOT_VOICE_OFF,
    EVT_DRUM_PARAM,
};

enum DrumId : uint8_t { DRUM_KICK = 0, DRUM_SNARE = 1, DRUM_HAT = 2 };
enum FxId : uint8_t {
    FX_BEAT_REPEAT = 0, // beat repeat: división controlada por PARAM_BEAT_REPEAT_DIV
    FX_FREEZE,          // grain freeze momentáneo
    FX_OCT_DOWN,
    FX_OCT_UP,
    FX_VIBRATO,
    FX_REPEAT_16,       // beat repeat forzado a 1/16
    FX_REPEAT_8,        // beat repeat forzado a 1/8
    FX_REPEAT_4,        // beat repeat forzado a 1/4
};

// Targets especiales para EVT_AFTERTOUCH (ev.target):
//   Nota MIDI (0–127)       → Note Mode velocity en tiempo real
//   PAD_MUTE (5)            → grain freeze wet (palma sobre MUTE)
//   AT_TARGET_REVERB_SNAP   → reverb wet momentáneo (SNAP pads)
static constexpr uint8_t AT_TARGET_REVERB_SNAP = 0xFD;

enum DrumParam : uint8_t {
    DRUM_PARAM_COLOR = 0,
    DRUM_PARAM_DECAY = 1,
    DRUM_PARAM_DUCK  = 2
};

enum ParamId : uint8_t {
    PARAM_MACRO = 0,
    PARAM_TONAL,
    PARAM_SPREAD,
    PARAM_DRIVE,
    PARAM_TIME_DIV,
    PARAM_SNAP_GATE,

    PARAM_GLIDE,
    PARAM_ENV_ATTACK,
    PARAM_ENV_RELEASE,
    PARAM_BEAT_REPEAT_DIV,  // V1.18: división rítmica del beat repeat (era STUTTER_RATE)
    PARAM_GRAIN,
    PARAM_HP,

    PARAM_REVERB_ROOM,
    PARAM_REVERB_WET,
    PARAM_CHORUS,
    PARAM_DRUM_DECAY,
    PARAM_DRUM_COLOR,
    PARAM_DUCK_AMOUNT,

    // V1.17: Delay tempo-sync
    PARAM_DELAY_DIV,    // divisor rítmico (0..1 → 11 pasos)
    PARAM_DELAY_FB,     // feedback 0..90%
    PARAM_DELAY_WET,    // wet amount 0..1

    // BYT3 parametric bytebeat engine
    PARAM_FORMULA_A,
    PARAM_FORMULA_B,
    PARAM_MORPH,
    PARAM_RATE,
    PARAM_SHIFT,
    PARAM_MASK,
    PARAM_FEEDBACK,
    PARAM_JITTER,
    PARAM_PHASE,
    PARAM_XOR_FOLD,
    PARAM_BB_SEED,
    PARAM_FILTER_MACRO,
    PARAM_RESONANCE,
    PARAM_ENV_MACRO,
};

static constexpr uint8_t PARAM_LOCK_LAST = PARAM_ENV_MACRO;

static constexpr uint32_t ROLL_THRESHOLD_MS = 120;

struct SequencerEvent {
    uint32_t  tick;
    EventType type;
    uint8_t   target;
    float     value;
};
