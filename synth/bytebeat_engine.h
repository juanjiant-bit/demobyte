
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
    enum AlgoKind : uint8_t {
        BYTE_LOGIC = 0,
        FLOAT_PATTERN = 1,
        FLOAT_PERC = 2,
        HYBRID = 3
    };

    struct AlgoSlot {
        AlgoKind kind;
        uint8_t variant;
    };

    float eval_slot(const AlgoSlot& slot, float ph, float rate_hz, float live, float zone, float morph) const;
    float eval_byte_logic(uint8_t variant, uint32_t t) const;
    float eval_float_pattern(uint8_t variant, float ph, float rate_hz, float live, float zone) const;
    float eval_float_perc(uint8_t variant, float ph, float rate_hz, float live, float zone) const;
    float eval_hybrid(uint8_t variant, uint32_t t, float ph, float rate_hz, float live, float zone, float morph) const;
    float softclip(float x) const;
    float rand01();

    float phase_ = 0.0f;
    float aux_phase_ = 0.0f;
    float aux2_phase_ = 0.0f;
    float perc_env_a_ = 0.0f;
    float perc_env_b_ = 0.0f;

    AlgoSlot slot_a_{BYTE_LOGIC, 0};
    AlgoSlot slot_b_{FLOAT_PATTERN, 0};

    float morph_ = 0.0f;
    float color_ = 0.0f;
    float mod_ = 0.0f;
    float pressure_ = 0.0f;
    bool drone_on_ = false;
    uint32_t lfsr_ = 0x13579BDFu;
};

}  // namespace synth
