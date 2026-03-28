
#include "drums/drum_engine.h"
#include <cmath>

namespace drums {
namespace {
static inline float softclip(float x) {
    return x / (1.0f + 0.85f * fabsf(x));
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

    // Kick rehecho: menos sweep raro, más seno/sub y click corto.
    if (kick_env_ > 0.0005f) {
        const float env = kick_env_;
        const float freq = 48.0f + 58.0f * env;   // drop corto y musical
        kick_phase_ += freq / 44100.0f;
        if (kick_phase_ >= 1.0f) kick_phase_ -= 1.0f;

        const float body = sinf(6.2831853f * kick_phase_);
        const float sub  = sinf(3.1415926f * kick_phase_) * 0.82f;
        const float click = (env > 0.90f) ? ((env - 0.90f) * 1.2f) : 0.0f;

        float k = (body * 0.58f + sub + click) * env;
        k = softclip(k * 1.9f);
        out += k * 1.28f;

        kick_env_ *= 0.99915f;
    }

    // Snare BUENA, apenas más presente.
    if (snare_env_ > 0.0005f) {
        float n = white(noise_);
        snare_tone_phase_ += (180.0f + 120.0f * color) / 44100.0f;
        if (snare_tone_phase_ >= 1.0f) snare_tone_phase_ -= 1.0f;
        float tone = sinf(6.2831853f * snare_tone_phase_) * 0.22f;
        out += (n * 0.46f + tone) * snare_env_;
        snare_env_ *= (0.965f - 0.04f * color);
    }

    // Hat BUENA, un poco más audible y menos click seco.
    if (hat_env_ > 0.0005f) {
        float n = white(noise_);
        float bright = (n >= 0.0f ? 1.0f : -1.0f) * sqrtf(fabsf(n));
        out += bright * hat_env_ * (0.18f + 0.07f * color);
        hat_env_ *= (0.90f - 0.05f * color);
    }

    return softclip(out);
}

}  // namespace drums
