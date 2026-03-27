#include "drums/drum_engine.h"
#include <cmath>

namespace drums {
namespace {
static inline float softclip(float x) {
    return x / (1.0f + 0.8f * fabsf(x));
}

static inline float white(unsigned& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return (float)(s & 0xFFFFu) * (1.0f / 32767.5f) - 1.0f;
}
}

void DrumEngine::init() {
    kick_env_ = 0.0f;
    snare_env_ = 0.0f;
    hat_env_ = 0.0f;
    kick_phase_ = 0.0f;
    snare_tone_phase_ = 0.0f;
}

void DrumEngine::trigger_kick() { kick_env_ = 1.0f; }
void DrumEngine::trigger_snare() { snare_env_ = 1.0f; }
void DrumEngine::trigger_hat() { hat_env_ = 1.0f; }

float DrumEngine::kick_env() const { return kick_env_; }

float DrumEngine::render(float color) {
    float out = 0.0f;

    if (kick_env_ > 0.0005f) {
        const float freq = 45.0f + 85.0f * kick_env_ + 18.0f * color;
        kick_phase_ += freq / 44100.0f;
        if (kick_phase_ >= 1.0f) kick_phase_ -= 1.0f;
        float body = sinf(6.2831853f * kick_phase_);
        float click = (kick_env_ > 0.7f) ? 0.18f : 0.0f;
        out += (body * 0.95f + click) * kick_env_;
        kick_env_ *= 0.9922f;
    }

    if (snare_env_ > 0.0005f) {
        float n = white(noise_);
        snare_tone_phase_ += (180.0f + 120.0f * color) / 44100.0f;
        if (snare_tone_phase_ >= 1.0f) snare_tone_phase_ -= 1.0f;
        float tone = sinf(6.2831853f * snare_tone_phase_) * 0.22f;
        out += (n * 0.42f + tone) * snare_env_;
        snare_env_ *= (0.905f - 0.08f * color);
    }

    if (hat_env_ > 0.0005f) {
        float n = white(noise_);
        // crude bright hat: subtract a bit of previous low tendency by shaping with sign
        float bright = (n >= 0.0f ? 1.0f : -1.0f) * fabsf(n);
        out += bright * hat_env_ * (0.14f + 0.06f * color);
        hat_env_ *= (0.76f - 0.10f * color);
    }

    return softclip(out);
}

}  // namespace drums
