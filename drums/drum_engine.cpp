#include "drums/drum_engine.h"
#include <cmath>
#include <cstdint>

namespace drums {

static inline float softclip(float x) {
    return x / (1.0f + 0.8f * fabsf(x));
<<<<<<< HEAD
}

static inline float rand_noise(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return (float)(s & 0xFFFFu) * (1.0f / 32767.5f) - 1.0f;
}
=======
>>>>>>> 86037eed4381e785b4aedc3cd98db0f0a6ee6e70
}

void DrumEngine::init() {
    kick_env_ = 0.0f;
    snare_env_ = 0.0f;
    hat_env_ = 0.0f;
    kick_phase_ = 0.0f;
    noise_ = 22222;
}

// =====================
// TRIGGERS
// =====================

void DrumEngine::trigger_kick()  { kick_env_ = 1.0f; }
void DrumEngine::trigger_snare() { snare_env_ = 1.0f; }
void DrumEngine::trigger_hat()   { hat_env_ = 1.0f; }

float DrumEngine::kick_env() const { return kick_env_; }

// =====================
// RENDER
// =====================

static inline float noise(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return (float)(s & 0xFFFF) / 32767.5f - 1.0f;
}

float DrumEngine::render(float color) {

    float out = 0.0f;

<<<<<<< HEAD
    // Kick simple y sólido
    if (kick_env_ > 0.0005f) {
        float env = kick_env_;
        float freq = 50.0f + 70.0f * env;
=======
    // =====================
    // KICK (REHECHO BIEN)
    // =====================
    if (kick_env_ > 0.0001f) {

        float env = kick_env_;

        // 🔥 pitch drop corto (NO sweep raro)
        float freq = 45.0f + 90.0f * env * env;

>>>>>>> 86037eed4381e785b4aedc3cd98db0f0a6ee6e70
        kick_phase_ += freq / 44100.0f;
        if (kick_phase_ >= 1.0f) kick_phase_ -= 1.0f;

        float sine = sinf(6.2831853f * kick_phase_);
<<<<<<< HEAD
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
=======

        // 🔥 sub más fuerte
        float sub = sinf(3.1415926f * kick_phase_) * 1.2f;

        // 🔥 click muy corto
        float click = (env > 0.85f) ? (env - 0.85f) * 4.0f : 0.0f;

        float k = (sine * 0.6f + sub + click) * env;

        // 🔥 saturación controlada
        k *= 2.2f;
        k = softclip(k);

        out += k * 2.2f; // 🔥 MÁS VOLUMEN

        // decay más musical
        kick_env_ *= 0.9975f;
    }

    // =====================
    // SNARE
    // =====================
    if (snare_env_ > 0.0001f) {

        float n = noise(noise_);
        float tone = sinf(snare_env_ * 180.0f);

        float s = (n * 0.8f + tone * 0.2f) * snare_env_;

        s *= 2.5f;
        out += softclip(s);

        snare_env_ *= 0.992f;
    }

    // =====================
    // HIHAT
    // =====================
    if (hat_env_ > 0.0001f) {

        float n = noise(noise_);

        float h = (n > 0 ? 1.0f : -1.0f) * hat_env_;

        h *= 2.0f;
        out += softclip(h);

        hat_env_ *= 0.985f;
>>>>>>> 86037eed4381e785b4aedc3cd98db0f0a6ee6e70
    }

    return softclip(out);
}

} // namespace drums
