
#pragma once
#include <stdint.h>

namespace synth {

class BytebeatEngine {
public:
    void init();
    void set_morph(float x);
    void set_color(float x);   // aftertouch live color / brightness
    void set_mod(float x);     // pot 3: wide rate sweep + zone modulation
    void set_drone(bool on);
    void set_pressure(float x);
    void next_formula_pair();
    void randomize_on_boot();
    float render();

private:
    float eval_formula(uint8_t id, uint32_t t) const;
    float softclip(float x) const;
    float zone_shape(float x, int zone, float zf) const;

    float phase_ = 0.0f;
    uint8_t formula_a_ = 0;
    uint8_t formula_b_ = 1;
    float morph_ = 0.0f;
    float color_ = 0.0f;
    float mod_ = 0.0f;
    float pressure_ = 0.0f;
    bool drone_on_ = false;
    uint32_t lfsr_ = 0x13579BDFu;
};

}  // namespace synth
