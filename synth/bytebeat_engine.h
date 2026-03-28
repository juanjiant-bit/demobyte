
#pragma once
#include <stdint.h>

namespace synth {

class BytebeatEngine {
public:
    void init();
    void set_morph(float x);
    void set_color(float x);   // bright / tone
    void set_mod(float x);     // wide rate / complexity macro
    void set_drone(bool on);
    void set_pressure(float x);
    void next_formula_pair();
    void randomize_on_boot();
    float render();

private:
    float eval_formula(uint8_t id, uint32_t t) const;
    float softclip(float x) const;
    float rand01();
    uint8_t pick_formula();

    float phase_ = 0.0f;
    float sub_phase_ = 0.0f;

    uint8_t formula_a_ = 0;
    uint8_t formula_b_ = 1;

    uint8_t history_[6] = {255,255,255,255,255,255};
    uint8_t hist_pos_ = 0;

    float morph_ = 0.0f;
    float color_ = 0.0f;
    float mod_ = 0.0f;
    float pressure_ = 0.0f;
    bool drone_on_ = true;
    uint32_t lfsr_ = 0x13579BDFu;
};

}  // namespace synth
