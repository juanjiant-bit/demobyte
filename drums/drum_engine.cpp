
#include "drums/drum_engine.h"
#include <cmath>

namespace drums {
namespace {
static inline float softclip(float x) {
    return x / (1.0f + 1.05f * fabsf(x));
}
static inline float sat(float x) {
    return x / (1.0f + 1.9f * fabsf(x));
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

    // KICK REHECHO:
    // - menos sweep raro
    // - más seno/sub
    // - click corto separado
    // - saturación local tipo drum bus
    if (kick_env_ > 0.0005f) {
        const float env = kick_env_;

        // pitch drop más controlado y corto
        const float freq = 43.0f + 92.0f * env * env + 6.0f * color;
        kick_phase_ += freq / 44100.0f;
        if (kick_phase_ >= 1.0f) kick_phase_ -= 1.0f;

        const float s1 = sinf(6.2831853f * kick_phase_);
        const float sub = sinf(3.1415926f * kick_phase_) * 0.85f;

        // click muy corto al inicio, no metálico
        const float click_env = (env > 0.86f) ? ((env - 0.86f) / 0.14f) : 0.0f;
        const float click = click_env * 0.10f;

        float k = (s1 * 0.62f + sub + click) * env;
        k = sat(k * 2.6f);
        out += k * 1.45f;

        // decay más musical, relativamente seco
        kick_env_ *= 0.99915f;
    }

    // SNARE
    if (snare_env_ > 0.0005f) {
        float n = white(noise_);
        snare_tone_phase_ += (150.0f + 110.0f * color) / 44100.0f;
        if (snare_tone_phase_ >= 1.0f) snare_tone_phase_ -= 1.0f;

        float tone = sinf(6.2831853f * snare_tone_phase_) * 0.20f;
        float s = (n * 0.72f + tone) * snare_env_;
        s = sat(s * 1.9f);
        out += s * 1.00f;

        snare_env_ *= (0.9979f - 0.0005f * color);
    }

    // HIHAT
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
