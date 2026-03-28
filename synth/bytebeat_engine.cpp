
#include "synth/bytebeat_engine.h"
#include <algorithm>
#include <cmath>

namespace synth {

static inline float clamp01(float x){
    return std::clamp(x, 0.0f, 1.0f);
}

void BytebeatEngine::init(){
    t_ = 0;
    formula_a_ = 0;
    formula_b_ = 1;
    morph_ = 0.0f;
    color_ = 0.0f;
    pressure_ = 0.0f;
    drone_on_ = true;
}

void BytebeatEngine::set_morph(float x){ morph_ = clamp01(x); }
void BytebeatEngine::set_color(float x){ color_ = clamp01(x); }
void BytebeatEngine::set_pressure(float x){ pressure_ = clamp01(x); }
void BytebeatEngine::set_drone(bool on){ drone_on_ = on; }

void BytebeatEngine::next_formula_pair(){
    formula_a_ = (formula_a_ + 1) % 6;
    formula_b_ = (formula_b_ + 2) % 6;
}

float BytebeatEngine::eval_formula(uint8_t id, uint32_t t) const {
    switch(id % 6){
        default:
        case 0: return (float)((t>>5)|(t>>8)) / 128.0f - 1.0f;
        case 1: return (float)((t*3 & t>>10)) / 128.0f - 1.0f;
        case 2: return (float)((t>>4)*(t>>7)) / 128.0f - 1.0f;
        case 3: return sinf(t*0.002f);
        case 4: return sinf(t*0.001f)*0.5f;
        case 5: return (float)((t>>6)^(t>>9)) / 128.0f - 1.0f;
    }
}

float BytebeatEngine::render(){

    // 🔥 BAJAMOS MUCHO EL CLOCK (esto mata la nota aguda)
    uint32_t step = 1 + (uint32_t)(color_ * 3.0f);
    t_ += (step >> 1);

    float a = eval_formula(formula_a_, t_);
    float b = eval_formula(formula_b_, t_);

    float x = a + (b - a) * morph_;

    // 🔥 SACAMOS RINGMOD (era esto)
    // viejo: hp fuerte
    float prev = eval_formula(formula_a_, t_ > 0 ? t_-1 : 0);
    float hp = x - 0.10f * prev;
    x = x * 0.75f + hp * 0.25f;

    // 🔥 FILTRO PARA MATAR MOSQUITO
    static float lp = 0.0f;
    lp += 0.08f * (x - lp);
    x = lp;

    return x;
}

}
