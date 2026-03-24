#pragma once
#include <cstdint>
#include "oled_display.h"
#include "../sequencer/event_types.h"
#include "../state/encoder_state.h"
#include "../synth/quantizer.h"

class UiRenderer {
public:
    void init(OledDisplay* display);
    void update(uint32_t now_ms);

    void set_status(uint8_t active_slot,
                    float bpm,
                    bool playing,
                    bool recording,
                    bool shift,
                    bool shift_rec,
                    bool note_mode,
                    bool snapshot_morph,
                    bool split_output,
                    const EncoderState& encoder_state,
                    bool note_active = false,
                    uint8_t note_degree = 0xFFu,
                    uint8_t note_midi = 0xFFu,
                    uint8_t note_voice_source = 0u);

    void show_message(const char* line1, uint32_t duration_ms = 1200, const char* line2 = nullptr);
    void show_action_message(const char* action, uint32_t duration_ms = 1200);
    void show_parameter(ParamId param, float value, const char* detail = nullptr, uint32_t duration_ms = 1000);

private:
    OledDisplay* display_ = nullptr;
    uint32_t last_update_ms_ = 0;
    static constexpr uint32_t UPDATE_MS = 66;

    uint8_t active_slot_ = 0;
    float bpm_ = 120.0f;
    bool playing_ = false;
    bool recording_ = false;
    bool shift_ = false;
    bool shift_rec_ = false;
    bool note_mode_ = false;
    bool snapshot_morph_ = false;
    bool split_output_ = false;
    EncoderState encoder_state_ = {};
    bool note_active_ = false;
    uint8_t note_degree_ = 0xFFu;
    uint8_t note_midi_ = 0xFFu;
    uint8_t note_voice_source_ = 0u;

    char msg1_[22] = {};
    char msg2_[22] = {};
    uint32_t msg_until_ms_ = 0;

    char param_name_[22] = {};
    char param_value_[22] = {};
    uint32_t param_until_ms_ = 0;

    char last_line1_[22] = {};
    char last_line2_[22] = {};
    char last_line3_[22] = {};

    void redraw(uint32_t now_ms);
    static const char* encoder_mode_name(EncoderMode mode);
    static const char* param_name(ParamId id);
    static void format_value(char* out, size_t n, ParamId id, float value, const char* detail);
};
