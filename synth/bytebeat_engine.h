#pragma once
#include <stdint.h>

namespace synth {

struct Params {
    float macro = 0.0f;
    float morph = 0.0f;
    float tone = 0.0f;
    float drive = 0.0f;
};

class BytebeatEngine {
public:
    void init();
    void set_macro(float m);
    void randomize_formula();
    void set_drone(bool on);
    float render();

private:
    float eval_formula(uint8_t id, uint32_t t) const;
    float softclip(float x) const;

    uint32_t t_ = 0;
    uint8_t formula_a_ = 0;
    uint8_t formula_b_ = 1;
    Params p_{};
    bool drone_on_ = false;
};

}  // namespace synth
