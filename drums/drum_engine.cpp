
#include "drums/drum_engine.h"
#include <cmath>

namespace drums {
namespace {
static inline float softclip(float x) {
    return x / (1.0f + 0.9f * fabsf(x));
}

static inline float sat(float x) {
    return x / (1.0f + 1.45f * fabsf(x));
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

    // Kick reformulado: más seno/sub, menos sweep raro.
    if (kick_env_ > 0.0005f) {
        const float env = kick_env_;
        const float freq = 47.0f + 58.0f * env + 6.0f * color;
        kick_phase_ += freq / 44100.0f;
        if (kick_phase_ >= 1.0f) kick_phase_ -= 1.0f;

        const float body = sinf(6.2831853f * kick_phase_);
        const float sub  = sinf(3.1415926f * kick_phase_) * 0.72f;
        const float click = (env > 0.90f) ? ((env - 0.90f) * 0.9f) : 0.0f;

        float k = (body * 0.68f + sub + click) * env;
        k = sat(k * 2.05f);
        out += k * 1.12f;

        kick_env_ *= 0.99895f;
    }

    if (snare_env_ > 0.0005f) {
        float n = white(noise_);
        snare_tone_phase_ += (165.0f + 95.0f * color) / 44100.0f;
        if (snare_tone_phase_ >= 1.0f) snare_tone_phase_ -= 1.0f;

        float tone = sinf(6.2831853f * snare_tone_phase_) * 0.18f;
        float s = (n * 0.60f + tone) * snare_env_;
        s = sat(s * 1.75f);
        out += s * 0.95f;

        snare_env_ *= (0.9978f - 0.0004f * color);
    }

    if (hat_env_ > 0.0005f) {
        float n = white(noise_);
        float bright = (n >= 0.0f ? 1.0f : -1.0f) * sqrtf(fabsf(n));
        float h = bright * hat_env_ * (0.20f + 0.10f * color);
        h = sat(h * 1.9f);
        out += h * 0.74f;

        hat_env_ *= (0.9926f - 0.0008f * color);
    }

    return softclip(out);
}

}  // namespace drums
