
#include "drums/drum_engine.h"
#include <cmath>
#include <cstdint>

namespace drums {
namespace {
static inline float softclip(float x) {
    return x / (1.0f + 0.8f * fabsf(x));
}

static inline float rand_noise(uint32_t& s) {
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

    // Kick simple y sólido
    if (kick_env_ > 0.0005f) {
        float env = kick_env_;
        float freq = 50.0f + 70.0f * env;
        kick_phase_ += freq / 44100.0f;
        if (kick_phase_ >= 1.0f) kick_phase_ -= 1.0f;

        float sine = sinf(6.2831853f * kick_phase_);
        float sub  = sinf(3.1415926f * kick_phase_) * 0.9f;
        float click = (env > 0.92f) ? (env - 0.92f) * 1.2f : 0.0f;

        float k = (sine * 0.6f + sub + click) * env;
        k = softclip(k * 1.8f);
        out += k * 1.5f;

        kick_env_ *= 0.9992f;
    }

    if (snare_env_ > 0.0005f) {
        float n = rand_noise(noise_);
        snare_tone_phase_ += (150.0f + 110.0f * color) / 44100.0f;
        if (snare_tone_phase_ >= 1.0f) snare_tone_phase_ -= 1.0f;

        float tone = sinf(6.2831853f * snare_tone_phase_) * 0.2f;
        float s = (n * 0.72f + tone) * snare_env_;
        out += softclip(s * 2.0f);

        snare_env_ *= (0.9979f - 0.0005f * color);
    }

    if (hat_env_ > 0.0005f) {
        float n = rand_noise(noise_);
        float bright = (n >= 0.0f ? 1.0f : -1.0f) * sqrtf(fabsf(n));
        float h = bright * hat_env_ * (0.26f + 0.12f * color);
        out += softclip(h * 2.2f);

        hat_env_ *= (0.9920f - 0.0007f * color);
    }

    return softclip(out);
}

}  // namespace drums
