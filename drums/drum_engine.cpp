#include "drums/drum_engine.h"
#include <cmath>

namespace drums {

static inline float softclip(float x) {
    return x / (1.0f + 0.9f * fabsf(x));
}

void DrumEngine::init() {
    kick_env_ = snare_env_ = hat_env_ = 0.0f;
    kick_phase_ = 0.0f;
}

void DrumEngine::trigger_kick() { kick_env_ = 1.0f; }
void DrumEngine::trigger_snare() { snare_env_ = 1.0f; }
void DrumEngine::trigger_hat() { hat_env_ = 1.0f; }

float DrumEngine::kick_env() const { return kick_env_; }

float DrumEngine::render(float macro) {
    float out = 0.0f;

    if (kick_env_ > 0.0005f) {
        float freq = 42.0f + 90.0f * kick_env_;
        kick_phase_ += freq / 44100.0f;
        if (kick_phase_ >= 1.0f) kick_phase_ -= 1.0f;
        out += sinf(6.2831853f * kick_phase_) * kick_env_ * (0.9f + 0.2f * macro);
        kick_env_ *= 0.9925f;
    }

    if (snare_env_ > 0.0005f) {
        noise_ ^= noise_ << 13;
        noise_ ^= noise_ >> 17;
        noise_ ^= noise_ << 5;
        float n = (float)(noise_ & 0xFFFFu) / 32768.0f - 1.0f;
        out += n * snare_env_ * 0.45f;
        snare_env_ *= (0.90f - 0.10f * macro);
    }

    if (hat_env_ > 0.0005f) {
        noise_ ^= noise_ << 13;
        noise_ ^= noise_ >> 17;
        noise_ ^= noise_ << 5;
        float n = (float)(noise_ & 0xFFFFu) / 32768.0f - 1.0f;
        out += n * hat_env_ * 0.18f;
        hat_env_ *= (0.78f - 0.12f * macro);
    }

    return softclip(out);
}

}  // namespace drums
