#include "master/master.h"
#include <cmath>
#include <algorithm>

namespace master {
namespace {
static inline float softclip(float x) {
    return x / (1.0f + 0.35f * fabsf(x));
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
    // DC block mucho más suave
    float hp = x - dc_x1_ + 0.9992f * hp_y1_;
    dc_x1_ = x;
    hp_y1_ = hp;

    // Sin compresión agresiva ni make-up desmedido
    hp *= 1.00f;
    hp *= volume_;

    return softclip(hp);
}

}  // namespace master
