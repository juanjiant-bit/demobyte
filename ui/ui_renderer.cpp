#include "ui_renderer.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include "pico/time.h"

namespace {
static const char* kFormulaNames[] = {
    "SCALE","LADDER","BROKARP","PENTA",
    "SIERPAD","WEAVE","CHOIR","DRMESH",
    "KLOGIC","SNARDST","HATCLK","GATEPRC",
    "XORRAIN","DSHARD","FOLDSTA","HBLOOM","BTAPE"
};
static const char* kFormulaTypes[] = {
    "TMEL","TMEL","TMEL","TMEL",
    "HARM","HARM","HARM","HARM",
    "PERC","PERC","PERC","PERC",
    "CHAO","CHAO","CHAO","HYBR","HYBR"
};
static const char* kMaskFamilies[] = {"LOFI","PULSE","PWM","CRUSH","FOLD","COMB","XOR","SHIMR"};
static const char* kEnvShapes[] = {"SWELL","PLUCK","GATE","ACCENT","TAIL"};
static const char* kFilterZones[] = {"LP","CLEAN","HP"};

static bool str_changed(char* dst, size_t n, const char* src) {
    if (std::strncmp(dst, src ? src : "", n) == 0) return false;
    std::snprintf(dst, n, "%s", src ? src : "");
    return true;
}
static const char* note_degree_name(uint8_t degree) {
    static const char* names[] = {"I","II","III","IV","V","VI","VII","VIII"};
    return (degree < 8u) ? names[degree] : "--";
}
static const char* note_source_name(uint8_t src) {
    switch (src) {
        case 0u: return "A";
        case 1u: return "B";
        case 2u: return "MORPH";
        default: return "?";
    }
}
}


void UiRenderer::init(OledDisplay* display) { display_ = display; }

void UiRenderer::set_status(uint8_t active_slot, float bpm, bool playing, bool recording,
                            bool shift, bool shift_rec, bool note_mode, bool snapshot_morph,
                            bool split_output, const EncoderState& encoder_state,
                            bool note_active, uint8_t note_degree, uint8_t note_midi, uint8_t note_voice_source) {
    active_slot_ = active_slot; bpm_ = bpm; playing_ = playing; recording_ = recording;
    shift_ = shift; shift_rec_ = shift_rec; note_mode_ = note_mode; snapshot_morph_ = snapshot_morph; split_output_ = split_output;
    encoder_state_ = encoder_state;
    note_active_ = note_active;
    note_degree_ = note_degree;
    note_midi_ = note_midi;
    note_voice_source_ = note_voice_source;
}

void UiRenderer::show_message(const char* line1, uint32_t duration_ms, const char* line2) {
    std::snprintf(msg1_, sizeof(msg1_), "%s", line1 ? line1 : "");
    std::snprintf(msg2_, sizeof(msg2_), "%s", line2 ? line2 : "");
    msg_until_ms_ = to_ms_since_boot(get_absolute_time()) + duration_ms;
}
void UiRenderer::show_action_message(const char* action, uint32_t duration_ms) { show_message(action, duration_ms, nullptr); }
void UiRenderer::show_parameter(ParamId param, float value, const char* detail, uint32_t duration_ms) {
    std::snprintf(param_name_, sizeof(param_name_), "%s", param_name(param));
    format_value(param_value_, sizeof(param_value_), param, value, detail);
    param_until_ms_ = to_ms_since_boot(get_absolute_time()) + duration_ms;
}

const char* UiRenderer::encoder_mode_name(EncoderMode mode) {
    switch (mode) {
        case EncoderMode::BPM: return "BPM";
        case EncoderMode::SWING: return "SWING";
        case EncoderMode::ROOT: return "ROOT";
        case EncoderMode::SCALE: return "SCALE";
        case EncoderMode::MUTATE: return "MUTATE";
        case EncoderMode::DENSITY: return "DENSITY";
        case EncoderMode::CHAOS: return "CHAOS";
        case EncoderMode::SPACE: return "SPACE";
        default: return "MODE";
    }
}
const char* UiRenderer::param_name(ParamId id) {
    switch (id) {
        case PARAM_FORMULA_A: return "FORM A";
        case PARAM_FORMULA_B: return "FORM B";
        case PARAM_MORPH: return "MORPH";
        case PARAM_RATE: return "RATE";
        case PARAM_SHIFT: return "SHIFT";
        case PARAM_MASK: return "MASK";
        case PARAM_FEEDBACK: return "FDBK";
        case PARAM_JITTER: return "JITTER";
        case PARAM_PHASE: return "PHASE";
        case PARAM_XOR_FOLD: return "XOR/FOLD";
        case PARAM_BB_SEED: return "SEED";
        case PARAM_FILTER_MACRO: return "FILTER";
        case PARAM_RESONANCE: return "RESON";
        case PARAM_ENV_MACRO: return "ENV";
        case PARAM_REVERB_ROOM: return "REV ROOM";
        case PARAM_REVERB_WET: return "REV WET";
        case PARAM_CHORUS: return "CHORUS";
        case PARAM_DRUM_DECAY: return "DRM DEC";
        case PARAM_DRUM_COLOR: return "DRM CLR";
        case PARAM_DUCK_AMOUNT: return "DUCK";
        case PARAM_DELAY_WET: return "DLY WET";
        default: return "PARAM";
    }
}
void UiRenderer::format_value(char* out, size_t n, ParamId id, float value, const char* detail) {
    if (detail && detail[0]) { std::snprintf(out, n, "%s", detail); return; }
    const int pct = (int)std::lround(value * 100.0f);
    switch (id) {
        case PARAM_FORMULA_A:
        case PARAM_FORMULA_B: {
            int idx = (int)std::lround(value * 16.0f);
            if (idx < 0) idx = 0; if (idx > 16) idx = 16;
            std::snprintf(out, n, "%02d %s/%s", idx, kFormulaNames[idx], kFormulaTypes[idx]);
            break;
        }
        case PARAM_MASK: {
            int fam = (int)(value * 7.99f);
            if (fam < 0) fam = 0; if (fam > 7) fam = 7;
            std::snprintf(out, n, "%s %d", kMaskFamilies[fam], pct);
            break;
        }
        case PARAM_FILTER_MACRO: {
            const char* zone = value < 0.45f ? kFilterZones[0] : (value > 0.55f ? kFilterZones[2] : kFilterZones[1]);
            std::snprintf(out, n, "%s %d", zone, pct);
            break;
        }
        case PARAM_ENV_MACRO: {
            int idx = (int)std::lround(value * 4.0f);
            if (idx < 0) idx = 0; if (idx > 4) idx = 4;
            std::snprintf(out, n, "%s %d", kEnvShapes[idx], pct);
            break;
        }
        case PARAM_RATE: {
            static const char* steps[] = {"1","2","3","4","6","8","12","16","24","32","48","64","96","128","192","256"};
            int idx = (int)std::lround(value * 15.0f);
            if (idx < 0) idx = 0; if (idx > 15) idx = 15;
            std::snprintf(out, n, "%sx", steps[idx]);
            break;
        }
        case PARAM_SHIFT: {
            int idx = (int)std::lround(value * 7.0f);
            if (idx < 0) idx = 0; if (idx > 7) idx = 7;
            std::snprintf(out, n, "%d", idx);
            break;
        }
        case PARAM_XOR_FOLD:
            if (value < 0.34f) std::snprintf(out, n, "CLEAN %d", pct);
            else if (value < 0.67f) std::snprintf(out, n, "XOR %d", pct);
            else std::snprintf(out, n, "FOLD %d", pct);
            break;
        case PARAM_RESONANCE:
        case PARAM_FEEDBACK:
        case PARAM_JITTER:
        case PARAM_PHASE:
        case PARAM_REVERB_ROOM:
        case PARAM_REVERB_WET:
        case PARAM_CHORUS:
        case PARAM_DRUM_DECAY:
        case PARAM_DRUM_COLOR:
        case PARAM_DUCK_AMOUNT:
        case PARAM_DELAY_WET:
        case PARAM_MORPH:
        case PARAM_BB_SEED:
        default:
            std::snprintf(out, n, "%d", pct);
            break;
    }
}

void UiRenderer::update(uint32_t now_ms) {
    if (!display_) return;
    if ((now_ms - last_update_ms_) < UPDATE_MS) return;
    last_update_ms_ = now_ms;
    redraw(now_ms);
    if (display_->dirty()) display_->update();
}

void UiRenderer::redraw(uint32_t now_ms) {
    char line1[22] = {};
    char line2[22] = {};
    char line3[22] = {};
    const bool msg_active = msg_until_ms_ > now_ms;
    const bool param_active = param_until_ms_ > now_ms;

    std::snprintf(line1, sizeof(line1), "S%02u %3.0f %s", (unsigned)(active_slot_ + 1), bpm_,
                  split_output_ ? "SPLIT" : (recording_ ? "REC" : (playing_ ? "PLAY" : "STOP")));

    if (msg_active) {
        std::snprintf(line2, sizeof(line2), "%s", msg1_);
        if (msg2_[0]) std::snprintf(line3, sizeof(line3), "%s", msg2_);
        else std::snprintf(line3, sizeof(line3), "%s", snapshot_morph_ ? "SNAP MORPH" : encoder_mode_name(encoder_state_.mode));
    } else {
        if (snapshot_morph_) std::snprintf(line2, sizeof(line2), "SNAP MORPH");
        else if (shift_rec_) std::snprintf(line2, sizeof(line2), "SHIFT+REC");
        else if (note_mode_) {
            if (note_active_ && note_midi_ != 0xFFu) {
                std::snprintf(line2, sizeof(line2), "NOTE %s %s", Quantizer::root_name(note_midi_ % 12u), Quantizer::scale_name(encoder_state_.scale_id));
            } else {
                std::snprintf(line2, sizeof(line2), "NOTE %s", Quantizer::scale_name(encoder_state_.scale_id));
            }
        }
        else if (shift_) std::snprintf(line2, sizeof(line2), "SHIFT");
        else std::snprintf(line2, sizeof(line2), "%s", encoder_mode_name(encoder_state_.mode));

        if (param_active) {
            std::snprintf(line3, sizeof(line3), "%s %s", param_name_, param_value_);
        } else {
            if (note_mode_) {
                if (note_active_ && note_degree_ != 0xFFu) {
                    std::snprintf(line3, sizeof(line3), "%s %s DEG%s", Quantizer::root_name(encoder_state_.root), note_source_name(note_voice_source_), note_degree_name(note_degree_));
                } else {
                    std::snprintf(line3, sizeof(line3), "%s %s", Quantizer::root_name(encoder_state_.root), note_source_name(note_voice_source_));
                }
            } else {
                std::snprintf(line3, sizeof(line3), "%s", shift_rec_ ? "DEEP" : (split_output_ ? "OUT SPLIT" : "READY"));
            }
        }
    }

    bool changed = false;
    changed |= str_changed(last_line1_, sizeof(last_line1_), line1);
    changed |= str_changed(last_line2_, sizeof(last_line2_), line2);
    changed |= str_changed(last_line3_, sizeof(last_line3_), line3);
    if (!changed) return;

    display_->clear();
    display_->draw_text(0, 0, line1);
    display_->draw_text(0, 11, line2);
    display_->draw_text(0, 22, line3);
}
