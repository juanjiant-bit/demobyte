#include <cstdio>
#include "pico/stdlib.h"
#include "audio/audio_output_i2s.h"
#include "io/pads.h"
#include "drums/drum_engine.h"

static audio::AudioOutputI2S g_i2s;
static drums::DrumEngine g_drums;

static inline int16_t f_to_i16(float x) {
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    return static_cast<int16_t>(x * 32767.0f);
}

int main() {
    stdio_init_all();
    sleep_ms(1200);

    controls::init();
    g_i2s.init();
    g_drums.init();

    absolute_time_t last = get_absolute_time();
    uint32_t last_print_ms = 0;

    while (true) {
        const absolute_time_t now = get_absolute_time();

        if (absolute_time_diff_us(last, now) >= 1000) {
            last = now;
            controls::update_1ms();

            const auto& p1 = controls::pad(0);
            const auto& p2 = controls::pad(1);
            const auto& p3 = controls::pad(2);
            const auto& p4 = controls::pad(3);

            // Mapeo de validación:
            // P1 kick, P2 snare, P3 hat, P4 perc
            if (p1.trigger) g_drums.trigger_kick();
            if (p2.trigger) g_drums.trigger_snare();
            if (p3.trigger) g_drums.trigger_hat();
            if (p4.trigger) g_drums.trigger_perc();

            // Potes:
            // GP26 volume
            // GP27 morph -> decay global
            // GP28 color -> tone / brightness
            g_drums.set_decay(controls::morph());
            g_drums.set_tone(controls::color());

            const uint32_t now_ms = to_ms_since_boot(now);
            if (now_ms - last_print_ms >= 100) {
                last_print_ms = now_ms;
                printf(
                    "P1 raw=%d pr=%.2f trig=%d gate=%d | "
                    "P2 raw=%d pr=%.2f trig=%d gate=%d | "
                    "P3 raw=%d pr=%.2f trig=%d gate=%d | "
                    "P4 raw=%d pr=%.2f trig=%d gate=%d\n",
                    p1.raw, p1.pressure, p1.trigger, p1.pressed,
                    p2.raw, p2.pressure, p2.trigger, p2.pressed,
                    p3.raw, p3.pressure, p3.trigger, p3.pressed,
                    p4.raw, p4.pressure, p4.trigger, p4.pressed
                );
            }
        }

        float x = g_drums.render();
        x *= controls::volume();

        int16_t s = f_to_i16(x);
        g_i2s.write(s, s);
    }
}
