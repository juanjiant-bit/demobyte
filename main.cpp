
#include "pico/stdlib.h"
#include "audio/audio_output_i2s.h"
#include "io/pads.h"
#include "synth/bytebeat_engine.h"
#include "drums/drum_engine.h"
#include "master/master.h"

static audio::AudioOutputI2S g_i2s;
static synth::BytebeatEngine g_synth;
static drums::DrumEngine g_drums;
static master::Master g_master;

static bool g_drone = true;

static inline int16_t f_to_i16(float x) {
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    return static_cast<int16_t>(x * 32767.0f);
}

int main() {
    stdio_init_all();

    controls::init();
    g_i2s.init();
    g_synth.init();
    g_drums.init();
    g_master.init();
    g_synth.set_drone(g_drone);

    absolute_time_t last = get_absolute_time();

    // Trigger derivado del flanco de subida de pressed.
    // No dependemos de p.trigger porque en esta versión es lo que está fallando.
    bool prev_pressed[4] = {false, false, false, false};

    while (true) {
        const absolute_time_t now = get_absolute_time();
        if (absolute_time_diff_us(last, now) >= 1000) {
            last = now;
            controls::update_1ms();

            const auto& p1 = controls::pad(0);
            const auto& p2 = controls::pad(1);
            const auto& p3 = controls::pad(2);
            const auto& p4 = controls::pad(3);

            const bool trig1 = p1.pressed && !prev_pressed[0];
            const bool trig2 = p2.pressed && !prev_pressed[1];
            const bool trig3 = p3.pressed && !prev_pressed[2];
            const bool trig4 = p4.pressed && !prev_pressed[3];

            prev_pressed[0] = p1.pressed;
            prev_pressed[1] = p2.pressed;
            prev_pressed[2] = p3.pressed;
            prev_pressed[3] = p4.pressed;

            if (trig1) {
                g_drone = !g_drone;
                g_synth.set_drone(g_drone);
                g_synth.next_formula_pair();
            }
            if (trig2) g_drums.trigger_kick();
            if (trig3) g_drums.trigger_snare();
            if (trig4) g_drums.trigger_hat();

            g_synth.set_morph(controls::morph());
            g_synth.set_color(controls::color());
            g_synth.set_pressure(p1.pressed ? p1.pressure : 0.0f);
            g_master.set_volume(controls::volume());
        }

        const float color = controls::color();
        float bb = g_synth.render();
        const float duck = 1.0f - 0.55f * g_drums.kick_env();
        float drum = g_drums.render(color);

        float mix = bb * duck * 0.96f + drum * 0.92f;
        mix = g_master.process(mix);

        const int16_t s = f_to_i16(mix);
        g_i2s.write(s, s);
    }
}
