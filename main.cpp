#include <stdio.h>
#include "pico/stdlib.h"
#include "audio/audio_output_i2s.h"
#include "io/pads.h"
#include "synth/bytebeat_engine.h"
#include "drums/drum_engine.h"
#include "master/master.h"
#include <algorithm>

static audio::AudioOutputI2S g_i2s;
static synth::BytebeatEngine g_synth;
static drums::DrumEngine g_drums;
static master::Master g_master;

static inline int16_t f_to_i16(float x) {
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    return static_cast<int16_t>(x * 32767.0f);
}

static inline float clamp01(float x) {
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

static inline float contact_metric(const controls::PadState& p) {
    // Pressure already works in your hardware, but trigger didn't.
    // Also use raw vs baseline to recover attack edges.
    float ratio = 0.0f;
    if (p.baseline > 4) {
        ratio = (float(int(p.raw) - int(p.baseline)) / (float)p.baseline) * 4.0f;
    }
    ratio = clamp01(ratio);
    return std::max(p.pressure, ratio);
}

int main() {
    stdio_init_all();

    controls::init();
    g_i2s.init();
    g_synth.init();
    g_drums.init();
    g_master.init();
    g_synth.set_drone(true);
    g_synth.randomize_on_boot();

    absolute_time_t last = get_absolute_time();

    bool prev_gate[4] = {false, false, false, false};
    constexpr float kTrigMetric = 0.10f;
    constexpr float kRelMetric  = 0.045f;

    while (true) {
        const absolute_time_t now = get_absolute_time();
        if (absolute_time_diff_us(last, now) >= 1000) {
            last = now;
            controls::update_1ms();

            const auto& p1 = controls::pad(0);
            const auto& p2 = controls::pad(1);
            const auto& p3 = controls::pad(2);
            const auto& p4 = controls::pad(3);

            const float m1 = contact_metric(p1);
            const float m2 = contact_metric(p2);
            const float m3 = contact_metric(p3);
            const float m4 = contact_metric(p4);

            const bool gate1 = m1 > kTrigMetric;
            const bool gate2 = m2 > kTrigMetric;
            const bool gate3 = m3 > kTrigMetric;
            const bool gate4 = m4 > kTrigMetric;

            const bool trig1 = gate1 && !prev_gate[0];
            const bool trig2 = gate2 && !prev_gate[1];
            const bool trig3 = gate3 && !prev_gate[2];
            const bool trig4 = gate4 && !prev_gate[3];

            prev_gate[0] = (m1 > kRelMetric) ? gate1 : false;
            prev_gate[1] = (m2 > kRelMetric) ? gate2 : false;
            prev_gate[2] = (m3 > kRelMetric) ? gate3 : false;
            prev_gate[3] = (m4 > kRelMetric) ? gate4 : false;

            if (trig1) g_synth.next_formula_pair();
            if (trig2) g_drums.trigger_kick();
            if (trig3) g_drums.trigger_snare();
            if (trig4) g_drums.trigger_hat();

            g_synth.set_morph(controls::morph());

            // Pad 1 aftertouch stays as live color / tape-down feel
            const float at = clamp01(p1.pressure);
            const float live_color = clamp01(1.0f - 0.90f * at);
            g_synth.set_color(live_color);
            g_synth.set_pressure(at);

            // Pot 3 = wide musical sweep / complexity
            g_synth.set_mod(controls::color());

            g_master.set_volume(controls::volume());
        }

        const float drum_color = controls::color();
        float bb = g_synth.render();
        const float duck = 1.0f - 0.55f * g_drums.kick_env();
        float drum = g_drums.render(drum_color);

        float mix = bb * duck * 0.97f + drum * 0.90f;
        mix = g_master.process(mix);

        const int16_t s = f_to_i16(mix);
        g_i2s.write(s, s);
    }
}
