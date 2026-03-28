
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

    // IMPORTANTE:
    // Los decays anteriores eran demasiado rápidos para 44.1kHz
    // (0.9922 / 0.905 / 0.76 por sample), o sea casi clicks.
    // Acá se alargan a tiempos musicales reales.

    if (kick_env_ > 0.0005f) {
        const float freq = 38.0f + 95.0f * kick_env_ + 20.0f * color;
        kick_phase_ += freq / 44100.0f;
        if (kick_phase_ >= 1.0f) kick_phase_ -= 1.0f;

        float body = sinf(6.2831853f * kick_phase_);
        float sub  = sinf(3.1415926f * kick_phase_) * 0.32f;
        float click = (kick_env_ > 0.82f) ? 0.12f : 0.0f;

        out += (body * 1.05f + sub + click) * kick_env_;
        kick_env_ *= 0.99935f;   // muchísimo más largo y audible
    }

    if (snare_env_ > 0.0005f) {
        float n = white(noise_);
        snare_tone_phase_ += (160.0f + 150.0f * color) / 44100.0f;
        if (snare_tone_phase_ >= 1.0f) snare_tone_phase_ -= 1.0f;

        float tone = sinf(6.2831853f * snare_tone_phase_) * 0.28f;
        out += (n * 0.55f + tone) * snare_env_;
        snare_env_ *= (0.9981f - 0.0005f * color);
    }

    if (hat_env_ > 0.0005f) {
        float n = white(noise_);
        float bright = (n >= 0.0f ? 1.0f : -1.0f) * fabsf(n);
        out += bright * hat_env_ * (0.18f + 0.10f * color);
        hat_env_ *= (0.9948f - 0.0012f * color);
    }

    return softclip(out);
}

}  // namespace drums
