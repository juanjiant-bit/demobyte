#include "master/master.h"
#include <cmath>

namespace master {

static inline float softclip(float x) {
    return x / (1.0f + fabsf(x));
}

void Master::init() {
    dc_z_ = 0.0f;
    hp_z_ = 0.0f;
    env_ = 0.0f;
}

float Master::process(float x) {
    float hp = x - dc_z_ + 0.995f * hp_z_;
    dc_z_ = x;
    hp_z_ = hp;

    float a = fabsf(hp);
    env_ += 0.004f * (a - env_);

    float gain = 1.0f;
    if (env_ > 0.36f) {
        gain = 0.36f / env_;
    }

    hp *= gain;
    hp *= 1.75f;
    return softclip(hp);
}

}  // namespace master
