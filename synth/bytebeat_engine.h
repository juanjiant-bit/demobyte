#pragma once
#include <stdint.h>

namespace synth {

class BytebeatEngine {
public:
    void init();
    void set_morph(float x);
    void set_color(float x);
    void set_drone(bool on);
    void set_pressure(float x);
    void next_formula_pair();
    float render();

private:
    float eval_formula(uint8_t id, uint32_t t) const;
    float softclip(float x) const;

    uint32_t t_ = 0;
    uint8_t formula_a_ = 0;
    uint8_t formula_b_ = 1;
    float morph_ = 0.0f;
    float color_ = 0.0f;
    float pressure_ = 0.0f;
    bool drone_on_ = false;
    uint32_t lfsr_ = 0x13579BDFu;
};

}  // namespace synth
