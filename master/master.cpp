
#include "master/master.h"
#include <cmath>
#include <algorithm>

namespace master {
namespace {
static inline float softclip(float x) {
    return x / (1.0f + 0.55f * fabsf(x));
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
    // DC block MUY suave. Menos realce agudo que la versión anterior.
    float hp = x - dc_x1_ + 0.9992f * hp_y1_;
    dc_x1_ = x;
    hp_y1_ = hp;

    // Compresión mucho más suave
    const float a = fabsf(hp);
    env_ += 0.0018f * (a - env_);

    float gain = 1.0f;
    if (env_ > 0.55f) {
        gain = 0.55f / env_;
    }

    hp *= gain;

    // Menos make-up gain: este era uno de los grandes culpables del "mosquito"
    hp *= 1.15f;
    hp *= volume_;

    return softclip(hp);
}

}  // namespace master
