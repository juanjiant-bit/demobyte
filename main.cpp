
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
    const int d = int(p.raw) - int(p.baseline);
    if (d <= 0) return 0.0f;
    float by_delta = float(d) / 8.0f;
    float m = by_delta > p.pressure ? by_delta : p.pressure;
    if (m > 1.0f) m = 1.0f;
    return m;
}

struct Voice {
    bool active = false;
    float env = 0.0f;
    float phase = 0.0f;
    float freq = 220.0f;
    float decay = 0.995f;
    float amp = 0.8f;
    bool noise = false;
    unsigned rng = 0x12345678u;
};

static Voice v1, v2, v3, v4;

static inline float white(Voice& v) {
    v.rng ^= v.rng << 13;
    v.rng ^= v.rng >> 17;
    v.rng ^= v.rng << 5;
    return (float)(v.rng & 0xffffu) * (1.0f / 32767.5f) - 1.0f;
}

static void trigger_voice(Voice& v, float freq, float decay, float amp, bool noise=false) {
    v.active = true;
    v.env = 1.0f;
    v.freq = freq;
    v.decay = decay;
    v.amp = amp;
    v.noise = noise;
}

static float render_voice(Voice& v) {
    if (!v.active || v.env < 0.0005f) {
        v.active = false;
        return 0.0f;
    }
    float x;
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
    int dbg_counter = 0;

    constexpr float kTrig = 0.25f;
    constexpr float kRel  = 0.08f;

    while (true) {
        absolute_time_t now = get_absolute_time();
        if (absolute_time_diff_us(last_ctrl, now) >= 1000) {
            last_ctrl = now;
            controls::update_1ms();

            const auto& p1 = controls::pad(0);
            const auto& p2 = controls::pad(1);
            const auto& p3 = controls::pad(2);
            const auto& p4 = controls::pad(3);

            const float m1 = pad_metric(p1);
            const float m2 = pad_metric(p2);
            const float m3 = pad_metric(p3);
            const float m4 = pad_metric(p4);

            const bool g1 = m1 > kTrig;
            const bool g2 = m2 > kTrig;
            const bool g3 = m3 > kTrig;
            const bool g4 = m4 > kTrig;

            const bool t1 = g1 && !prev_gate[0];
            const bool t2 = g2 && !prev_gate[1];
            const bool t3 = g3 && !prev_gate[2];
            const bool t4 = g4 && !prev_gate[3];

            prev_gate[0] = (m1 > kRel) ? g1 : false;
            prev_gate[1] = (m2 > kRel) ? g2 : false;
            prev_gate[2] = (m3 > kRel) ? g3 : false;
            prev_gate[3] = (m4 > kRel) ? g4 : false;

            if (t1) trigger_voice(v1, 110.0f, 0.9980f, 0.80f, false);
            if (t2) trigger_voice(v2, 220.0f, 0.9968f, 0.70f, false);
            if (t3) trigger_voice(v3, 440.0f, 0.9955f, 0.60f, false);
            if (t4) trigger_voice(v4, 0.0f,   0.9750f, 0.45f, true);

            if (++dbg_counter >= 50) {
                dbg_counter = 0;
                printf("P1 raw=%d base=%d pr=%.2f m=%.2f trig=%d\n", p1.raw, p1.baseline, p1.pressure, m1, t1);
                printf("P2 raw=%d base=%d pr=%.2f m=%.2f trig=%d\n", p2.raw, p2.baseline, p2.pressure, m2, t2);
                printf("P3 raw=%d base=%d pr=%.2f m=%.2f trig=%d\n", p3.raw, p3.baseline, p3.pressure, m3, t3);
                printf("P4 raw=%d base=%d pr=%.2f m=%.2f trig=%d\n\n", p4.raw, p4.baseline, p4.pressure, m4, t4);
            }
        }

        float mix = 0.0f;
        mix += render_voice(v1);
        mix += render_voice(v2);
        mix += render_voice(v3);
        mix += render_voice(v4);
        mix = mix / (1.0f + fabsf(mix));

        int16_t s = f_to_i16(mix * 0.9f);
        g_i2s.write(s, s);
    }
}
