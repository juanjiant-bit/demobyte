#include <stdio.h>
#include <math.h>
#include "pico/stdlib.h"
#include "audio/audio_output_i2s.h"
#include "io/pads.h"

static audio::AudioOutputI2S g_i2s;

static inline float clamp1(float x) {
    if (x > 1.0f) return 1.0f;
    if (x < -1.0f) return -1.0f;
    return x;
}

static inline int16_t f_to_i16(float x) {
    return static_cast<int16_t>(clamp1(x) * 32767.0f);
}

static inline float pad_metric(const controls::PadState& p) {
    if (p.raw <= 25) {
        return p.pressure > 0.0f ? p.pressure : 0.0f;
    }
    float m = float(int(p.raw) - 25) / 75.0f;
    if (m < 0.0f) m = 0.0f;
    if (m > 1.0f) m = 1.0f;
    if (p.pressure > m) m = p.pressure;
    return m;
}

struct TickVoice {
    bool active = false;
    float env = 0.0f;
    float phase = 0.0f;
    float freq = 220.0f;
    float decay = 0.996f;
    float amp = 0.8f;
    bool noise = false;
    unsigned rng = 0x12345678u;
};

static TickVoice g_voice[4];

static inline float white(TickVoice& v) {
    v.rng ^= v.rng << 13;
    v.rng ^= v.rng >> 17;
    v.rng ^= v.rng << 5;
    return (float)(v.rng & 0xffffu) * (1.0f / 32767.5f) - 1.0f;
}

static void trigger_tick(int idx) {
    auto& v = g_voice[idx];
    v.active = true;
    v.env = 1.0f;
    switch (idx) {
        case 0: v.freq = 120.0f; v.decay = 0.9980f; v.amp = 0.85f; v.noise = false; break; // low tick
        case 1: v.freq = 260.0f; v.decay = 0.9970f; v.amp = 0.78f; v.noise = false; break; // mid tick
        case 2: v.freq = 520.0f; v.decay = 0.9960f; v.amp = 0.68f; v.noise = false; break; // high tick
        default: v.freq = 0.0f;  v.decay = 0.9820f; v.amp = 0.55f; v.noise = true;  break; // noise tick
    }
}

static float render_voice(TickVoice& v) {
    if (!v.active || v.env < 0.0005f) {
        v.active = false;
        return 0.0f;
    }

    float x = 0.0f;
    if (v.noise) {
        x = white(v);
    } else {
        v.phase += v.freq / 44100.0f;
        if (v.phase >= 1.0f) v.phase -= 1.0f;
        x = sinf(6.2831853f * v.phase);
    }

    x *= v.env * v.amp;
    v.env *= v.decay;
    return x;
}

int main() {
    stdio_init_all();
    sleep_ms(1200);

    controls::init();
    g_i2s.init();

    absolute_time_t last_ctrl = get_absolute_time();
    bool prev_gate[4] = {false, false, false, false};
    uint32_t lockout_ms[4] = {0, 0, 0, 0};

    constexpr float kTrig = 0.20f;
    constexpr float kRel  = 0.08f;
    constexpr uint32_t kLockout = 90;

    int dbg_counter = 0;

    while (true) {
        absolute_time_t now = get_absolute_time();

        if (absolute_time_diff_us(last_ctrl, now) >= 1000) {
            last_ctrl = now;
            controls::update_1ms();
            uint32_t now_ms = to_ms_since_boot(now);

            for (int i = 0; i < 4; ++i) {
                const auto& p = controls::pad(i);
                const float m = pad_metric(p);
                bool gate = m > kTrig;
                bool trig = false;

                if (now_ms > lockout_ms[i]) {
                    trig = gate && !prev_gate[i];
                }

                if (trig) {
                    lockout_ms[i] = now_ms + kLockout;
                    trigger_tick(i);
                }

                prev_gate[i] = (m > kRel) ? gate : false;
            }

            if (++dbg_counter >= 50) {
                dbg_counter = 0;
                const auto& p1 = controls::pad(0);
                const auto& p2 = controls::pad(1);
                const auto& p3 = controls::pad(2);
                const auto& p4 = controls::pad(3);
                printf("P1 raw=%d trig=%d press=%d hold=%d pr=%.2f\n", p1.raw, p1.trigger, p1.pressed, p1.held, p1.pressure);
                printf("P2 raw=%d trig=%d press=%d hold=%d pr=%.2f\n", p2.raw, p2.trigger, p2.pressed, p2.held, p2.pressure);
                printf("P3 raw=%d trig=%d press=%d hold=%d pr=%.2f\n", p3.raw, p3.trigger, p3.pressed, p3.held, p3.pressure);
                printf("P4 raw=%d trig=%d press=%d hold=%d pr=%.2f\n", p4.raw, p4.trigger, p4.pressed, p4.held, p4.pressure);
                printf("pots vol=%.2f morph=%.2f color=%.2f\n\n", controls::volume(), controls::morph(), controls::color());
            }
        }

        float mix = 0.0f;
        for (int i = 0; i < 4; ++i) mix += render_voice(g_voice[i]);

        // volumen solo para chequear que el pote anda; nunca mute total
        float vol = 0.15f + 0.85f * controls::volume();
        mix *= vol;
        mix = mix / (1.0f + fabsf(mix));

        int16_t s = f_to_i16(mix * 0.9f);
        g_i2s.write(s, s);
    }
}
