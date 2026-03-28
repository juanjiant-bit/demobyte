
#include "drums/drum_engine.h"
#include <cmath>

namespace drums {

void DrumEngine::init(){
    kick_env_ = 0;
    kick_phase_ = 0;
}

void DrumEngine::trigger_kick(){
    kick_env_ = 1.0f;
}

float DrumEngine::kick_env() const { return kick_env_; }

float DrumEngine::render(float color){

    float out = 0;

    if(kick_env_ > 0.0001f){
        float env = kick_env_;

        // 🔥 KICK SIMPLE REAL
        float freq = 55.0f + 45.0f * env;
        kick_phase_ += freq / 44100.0f;
        if(kick_phase_ > 1) kick_phase_ -= 1;

        float s = sinf(6.28318f * kick_phase_);
        float sub = sinf(3.1415f * kick_phase_) * 0.8f;

        float k = (s*0.6f + sub) * env;

        // soft clip
        k = k * 1.8f;
        k = k / (1.0f + fabsf(k));

        out += k * 1.5f;

        kick_env_ *= 0.9992f;
    }

    return out;
}

}
