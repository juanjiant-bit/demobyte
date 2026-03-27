#pragma once
#include <cstdint>

namespace synth {

struct EngineParams {
    float volume = 0.8f;
    float morph = 0.0f;
    float color = 0.5f;
    bool drone = true;
    float tape = 0.0f;
};

class BytebeatEngine {
public:
    void init();
    void next_formula_pair();
    float render(const EngineParams& p);

private:
    float render_formula(int id, uint32_t tt, float c) const;
    uint32_t t_ = 0;
    uint32_t rand_ = 0x12345678u;
    int formula_a_ = 0;
    int formula_b_ = 5;
};

} // namespace synth
