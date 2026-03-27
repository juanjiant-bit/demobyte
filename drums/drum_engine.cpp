#include "drum_engine.h"
#include <cmath>

namespace drums {
namespace {
constexpr float SR = 44100.0f;
static inline float sat(float x){ return x / (1.0f + fabsf(x)); }
static inline float frand(unsigned int& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return ((s & 0xFFFFu) / 32767.5f) - 1.0f;
}
}

void DrumEngine::init() {
    noise_ = 0x87654321u;
    kick_env_ = snare_env_ = hat_env_ = 0.0f;
    kick_phase_ = 0.0f;
}

void DrumEngine::trigger_kick() { kick_env_ = 1.0f; }
void DrumEngine::trigger_snare() { snare_env_ = 1.0f; }
void DrumEngine::trigger_hat() { hat_env_ = 1.0f; }

float DrumEngine::render(float color, float pressure2, float pressure3, float pressure4) {
    float out = 0.0f;

    if (kick_env_ > 0.0002f) {
        float pitch = 38.0f + kick_env_ * (110.0f + pressure2 * 55.0f) + color * 8.0f;
        kick_phase_ += pitch / SR;
        if (kick_phase_ >= 1.0f) kick_phase_ -= 1.0f;
        float s = sinf(kick_phase_ * 6.2831853f);
        float body = sinf(kick_phase_ * 3.1415926f) * 0.25f;
        out += (s + body) * kick_env_ * 1.35f;
        kick_env_ *= 0.9952f - color * 0.0004f;
    }

    if (snare_env_ > 0.0002f) {
        float n = frand(noise_);
        float tone = sinf(kick_phase_ * 6.2831853f * 5.2f) * (0.18f + pressure3 * 0.14f);
        out += (n * (0.58f + color * 0.20f) + tone) * snare_env_ * 0.85f;
        snare_env_ *= 0.9898f - pressure3 * 0.0015f;
    }

    if (hat_env_ > 0.0002f) {
        float n = frand(noise_);
        float h = n - (n * n * n) * 0.33f;
        out += h * hat_env_ * (0.42f + color * 0.18f + pressure4 * 0.12f);
        hat_env_ *= 0.975f - pressure4 * 0.002f;
    }

    return sat(out);
}

} // namespace drums
