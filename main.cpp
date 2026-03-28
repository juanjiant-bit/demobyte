
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

    while (true) {
        const absolute_time_t now = get_absolute_time();
        if (absolute_time_diff_us(last, now) >= 1000) {
            last = now;
            controls::update_1ms();

            const auto& p1 = controls::pad(0);
            const auto& p2 = controls::pad(1);
            const auto& p3 = controls::pad(2);
            const auto& p4 = controls::pad(3);

            if (p1.trigger) {
                g_drone = !g_drone;
                g_synth.set_drone(g_drone);
                g_synth.next_formula_pair();
            }
            if (p2.trigger) g_drums.trigger_kick();
            if (p3.trigger) g_drums.trigger_snare();
            if (p4.trigger) g_drums.trigger_hat();

            g_synth.set_morph(controls::morph());
            g_synth.set_color(controls::color());

            // Clave:
            // solo el pad 1 gobierna la presión del motor,
            // y se cancela completamente cuando hay pads de drums activos.
            const bool drum_pad_active =
                p2.pressed || p3.pressed || p4.pressed ||
                p2.trigger || p3.trigger || p4.trigger;

            const float synth_pressure =
                (p1.pressed && !drum_pad_active) ? p1.pressure : 0.0f;

            g_synth.set_pressure(synth_pressure);

            g_master.set_volume(controls::volume());
        }

        const float color = controls::color();

        float bb = g_synth.render();
        const float duck = 1.0f - 0.45f * g_drums.kick_env();
        float drum = g_drums.render(color);

        // Drums más presentes para que no queden enterrados.
        float mix = bb * duck * 0.92f + drum * 1.18f;
        mix = g_master.process(mix);

        const int16_t s = f_to_i16(mix);
        g_i2s.write(s, s);
    }
}
