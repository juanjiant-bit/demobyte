
#include "drums/drum_engine.h"
#include <algorithm>
#include <cmath>

namespace drums {
namespace {
static inline float clamp01(float x) {
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

static inline float softclip(float x) {
    return x / (1.0f + 0.85f * fabsf(x));
}

static inline float white(unsigned& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return (float)(s & 0xFFFFu) * (1.0f / 32767.5f) - 1.0f;
}
}  // namespace

void DrumEngine::init() {
    decay_ = 0.5f;
    tone_ = 0.5f;
    kick_env_ = 0.0f;
    kick_phase_ = 0.0f;
    snare_env_ = 0.0f;
    snare_phase_ = 0.0f;
    hat_env_ = 0.0f;
    hat_hp_z_ = 0.0f;
    perc_env_ = 0.0f;
    perc_phase_ = 0.0f;
    noise_ = 0x12345678u;
}

void DrumEngine::set_decay(float x) { decay_ = clamp01(x); }
void DrumEngine::set_tone(float x)  { tone_  = clamp01(x); }

// CLAVE:
// resetear fase en cada trigger evita el sweep/vibrato raro al re-disparar.
void DrumEngine::trigger_kick()  { kick_env_ = 1.0f; kick_phase_ = 0.0f; }
void DrumEngine::trigger_snare() { snare_env_ = 1.0f; snare_phase_ = 0.0f; }
void DrumEngine::trigger_hat()   { hat_env_ = 1.0f; hat_hp_z_ = 0.0f; }
void DrumEngine::trigger_perc()  { perc_env_ = 1.0f; perc_phase_ = 0.0f; }

float DrumEngine::render() {
    float out = 0.0f;

    // KICK
    if (kick_env_ > 0.0005f) {
        const float env = kick_env_;

        // Pitch envelope corto y musical.
        // Arranca arriba y cae rápido a grave real.
        const float pitch_env = env * env;
        const float freq = 46.0f + 105.0f * pitch_env + 6.0f * tone_;

        kick_phase_ += freq / 44100.0f;
        if (kick_phase_ >= 1.0f) kick_phase_ -= 1.0f;

        // Una sola senoide principal. El "sub" anterior con sin(pi*phase)
        // era parte del problema y generaba ese barrido raro.
        const float body = sinf(6.2831853f * kick_phase_);

        // Ligero refuerzo grave, pero coherente de fase.
        const float weight = 1.0f - std::min(1.0f, kick_phase_ * 1.6f);

        // Click muy corto, no tonal.
        const float click_env = (env > 0.90f) ? (env - 0.90f) / 0.10f : 0.0f;
        const float click = click_env * 0.06f;

        float k = body * (0.95f + 0.35f * weight);
        k += click;
        k *= env;

        out += softclip(k * 2.4f) * 1.10f;

        const float kick_decay = 0.9968f + decay_ * 0.0022f;
        kick_env_ *= kick_decay;
    }

    // SNARE
    if (snare_env_ > 0.0005f) {
        float n = white(noise_);
        snare_phase_ += (155.0f + 110.0f * tone_) / 44100.0f;
        if (snare_phase_ >= 1.0f) snare_phase_ -= 1.0f;

        float tone = sinf(6.2831853f * snare_phase_) * 0.18f;
        float s = (n * 0.80f + tone) * snare_env_;
        out += softclip(s * 1.7f) * 0.75f;

        const float sn_decay = 0.9915f + decay_ * 0.0065f;
        snare_env_ *= sn_decay;
    }

    // HAT
    if (hat_env_ > 0.0005f) {
        float n = white(noise_);
        float hp = n - hat_hp_z_;
        hat_hp_z_ += 0.20f * (n - hat_hp_z_);

        float h = hp * hat_env_ * (0.34f + 0.26f * tone_);
        out += softclip(h * 1.9f) * 0.58f;

        const float hat_decay = 0.969f + decay_ * 0.020f;
        hat_env_ *= hat_decay;
    }

    // PERC/TOM
    if (perc_env_ > 0.0005f) {
        perc_phase_ += (200.0f + 150.0f * tone_) / 44100.0f;
        if (perc_phase_ >= 1.0f) perc_phase_ -= 1.0f;

        float a = sinf(6.2831853f * perc_phase_);
        float b = sinf(12.5663706f * perc_phase_) * 0.22f;
        float p = (a + b) * perc_env_;
        out += softclip(p * 1.5f) * 0.62f;

        const float perc_decay = 0.988f + decay_ * 0.008f;
        perc_env_ *= perc_decay;
    }

    return softclip(out);
}

}  // namespace drums
