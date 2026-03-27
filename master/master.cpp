#include "master.h"
#include <cmath>

namespace master {
namespace {
static inline float softclip(float x){ return x / (1.0f + fabsf(x)); }
}

void Master::init() {
    hp_x1_ = hp_y1_ = env_ = 0.0f;
}

float Master::process(float x, float volume) {
    const float hp = x - hp_x1_ + 0.995f * hp_y1_;
    hp_x1_ = x;
    hp_y1_ = hp;

    const float a = fabsf(hp);
    env_ += 0.0028f * (a - env_);

    float gain = 1.0f;
    if (env_ > 0.45f) gain = 0.45f / env_;

    float y = hp * gain;
    y *= (1.85f * volume);
    return softclip(y);
}

} // namespace master
