
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
}

void DrumEngine::set_decay(float x) { decay_ = clamp01(x); }
void DrumEngine::set_tone(float x)  { tone_  = clamp01(x); }

void DrumEngine::trigger_kick()  { kick_env_  = 1.0f; }
void DrumEngine::trigger_snare() { snare_env_ = 1.0f; }
void DrumEngine::trigger_hat()   { hat_env_   = 1.0f; }
void DrumEngine::trigger_perc()  { perc_env_  = 1.0f; }

float DrumEngine::render() {
    float out = 0.0f;

    // KICK
    if (kick_env_ > 0.0005f) {
        const float env = kick_env_;
        const float freq = 42.0f + 78.0f * env * env + 8.0f * tone_;
        kick_phase_ += freq / 44100.0f;
        if (kick_phase_ >= 1.0f) kick_phase_ -= 1.0f;

        float sine = sinf(6.2831853f * kick_phase_);
        float sub  = sinf(3.1415926f * kick_phase_) * 0.85f;
        float click = (env > 0.88f) ? (env - 0.88f) * 1.6f : 0.0f;

        float k = (sine * 0.58f + sub + click * 0.08f) * env;
        out += softclip(k * 1.9f) * 0.95f;

        const float kick_decay = 0.9970f + decay_ * 0.0018f; // corto -> largo
        kick_env_ *= kick_decay;
    }

    // SNARE
    if (snare_env_ > 0.0005f) {
        float n = white(noise_);
        snare_phase_ += (150.0f + 120.0f * tone_) / 44100.0f;
        if (snare_phase_ >= 1.0f) snare_phase_ -= 1.0f;

        float tone = sinf(6.2831853f * snare_phase_) * 0.20f;
        float s = (n * 0.75f + tone) * snare_env_;
        out += softclip(s * 1.6f) * 0.72f;

        const float sn_decay = 0.9920f + decay_ * 0.0060f;
        snare_env_ *= sn_decay;
    }

    // HAT
    if (hat_env_ > 0.0005f) {
        float n = white(noise_);
        float hp = n - hat_hp_z_;
        hat_hp_z_ += 0.22f * (n - hat_hp_z_);
        float h = hp * hat_env_ * (0.30f + 0.25f * tone_);
        out += softclip(h * 1.8f) * 0.55f;

        const float hat_decay = 0.970f + decay_ * 0.020f;
        hat_env_ *= hat_decay;
    }

    // PERC / TOM
    if (perc_env_ > 0.0005f) {
        perc_phase_ += (220.0f + 180.0f * tone_) / 44100.0f;
        if (perc_phase_ >= 1.0f) perc_phase_ -= 1.0f;

        float a = sinf(6.2831853f * perc_phase_);
        float b = sinf(12.5663706f * perc_phase_) * 0.35f;
        float p = (a + b) * perc_env_;
        out += softclip(p * 1.4f) * 0.60f;

        const float perc_decay = 0.989f + decay_ * 0.008f;
        perc_env_ *= perc_decay;
    }

    return softclip(out * 0.95f);
}

}  // namespace drums
