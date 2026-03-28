
#include "drums/drum_engine.h"
#include <cmath>

namespace drums {
namespace {
static inline float softclip(float x) {
    return x / (1.0f + 0.95f * fabsf(x));
}
static inline float sat(float x) {
    return x / (1.0f + 1.6f * fabsf(x));
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
        const float freq = 31.0f + 82.0f * kick_env_ + 10.0f * color;
        kick_phase_ += freq / 44100.0f;
        if (kick_phase_ >= 1.0f) kick_phase_ -= 1.0f;

        float body = sinf(6.2831853f * kick_phase_);
        float sub  = sinf(3.1415926f * kick_phase_) * 0.62f;
        float click = (kick_env_ > 0.90f) ? 0.08f : 0.0f;

        float k = (body * 0.72f + sub + click) * kick_env_;
        k = sat(k * 2.25f);   // more clipped / punchy
        out += k * 1.30f;

        kick_env_ *= 0.9989f; // drier but still audible
    }

    if (snare_env_ > 0.0005f) {
        float n = white(noise_);
        snare_tone_phase_ += (145.0f + 120.0f * color) / 44100.0f;
        if (snare_tone_phase_ >= 1.0f) snare_tone_phase_ -= 1.0f;

        float tone = sinf(6.2831853f * snare_tone_phase_) * 0.22f;
        float s = (n * 0.70f + tone) * snare_env_;
        s = sat(s * 1.85f);
        out += s * 1.00f;

        snare_env_ *= (0.9979f - 0.0005f * color);
    }

    if (hat_env_ > 0.0005f) {
        float n = white(noise_);
        float bright = (n >= 0.0f ? 1.0f : -1.0f) * sqrtf(fabsf(n));
        float h = bright * hat_env_ * (0.26f + 0.12f * color);
        h = sat(h * 2.2f);
        out += h * 0.88f;

        hat_env_ *= (0.9920f - 0.0007f * color);
    }

    return softclip(out);
}

}  // namespace drums
