#include "pico/stdlib.h"
#include "hardware/timer.h"
#include "io/pads.h"
#include "synth/bytebeat_engine.h"
#include "drums/drum_engine.h"
#include "master/master.h"
#include "audio/audio_output_i2s.h"

// Esta base asume que mantenés TU implementación I2S validada previamente.
// Debe exponer una API equivalente a init() + write(left,right).
static audio::AudioOutputI2S g_i2s;
static synth::BytebeatEngine g_synth;
static drums::DrumEngine g_drums;
static master::Master g_master;

static bool g_drone = false;

static inline int16_t f_to_i16(float x) {
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    return static_cast<int16_t>(x * 32767.0f);
}

int main() {
    stdio_init_all();
    pads::init();
    g_i2s.init();
    g_synth.init();
    g_drums.init();
    g_master.init();

    absolute_time_t last = get_absolute_time();

    while (true) {
        absolute_time_t now = get_absolute_time();
        if (absolute_time_diff_us(last, now) >= 1000) {
            last = now;
            pads::update_1ms();

            const auto& p1 = pads::get(0);
            const auto& p2 = pads::get(1);
            const auto& p3 = pads::get(2);
            const auto& p4 = pads::get(3);

            if (p1.trigger) {
                g_drone = !g_drone;
                g_synth.set_drone(g_drone);
                g_synth.randomize_formula();
            }
            if (p2.trigger) g_drums.trigger_kick();
            if (p3.trigger) g_drums.trigger_snare();
            if (p4.trigger) g_drums.trigger_hat();

            float macro = pads::macro();
            if (p1.pressed) {
                macro = macro * 0.75f + p1.pressure * 0.25f;
            }
            g_synth.set_macro(macro);
        }

        float macro = pads::macro();
        float bb = g_synth.render();
        float kick_duck = 1.0f - 0.58f * g_drums.kick_env();
        float drum = g_drums.render(macro);

        float mix = bb * kick_duck * 0.95f + drum * 0.95f;
        mix = g_master.process(mix);

        int16_t s = f_to_i16(mix);
        g_i2s.write(s, s);
    }
}
