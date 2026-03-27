#include "master/master.h"
#include <cmath>
#include <algorithm>

namespace master {
namespace {
static inline float softclip(float x) {
    return x / (1.0f + fabsf(x));
}
}

void Master::init() {
    dc_x1_ = 0.0f;
    hp_y1_ = 0.0f;
    env_ = 0.0f;
    volume_ = 1.0f;
}

void Master::set_volume(float x) {
    volume_ = std::clamp(x, 0.03f, 1.0f);
}

float Master::process(float x) {
    // DC-block / gentle HP
    float hp = x - dc_x1_ + 0.995f * hp_y1_;
    dc_x1_ = x;
    hp_y1_ = hp;

    // simple compressor
    const float a = fabsf(hp);
    env_ += 0.0045f * (a - env_);
    float gain = 1.0f;
    if (env_ > 0.34f) {
        gain = 0.34f / env_;
    }

    hp *= gain;
    hp *= 1.90f;   // make-up gain
    hp *= volume_;
    return softclip(hp);
}

}  // namespace master
