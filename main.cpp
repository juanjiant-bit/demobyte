#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "audio/audio_output_i2s.h"
#include "io/pads.h"
#include "synth/bytebeat_engine.h"
#include "drums/drum_engine.h"
#include "master/master.h"

using audio::AudioOutputI2S;

namespace {
constexpr uint LED_PIN = 25;

// Hardware real validado: 1 solo pote en GP26 / ADC0
constexpr uint ADC_MACRO_PIN = 26;

static inline float clamp01(float x) {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

uint16_t read_adc_avg(uint input) {
    adc_select_input(input);
    uint32_t s = 0;
    for (int i = 0; i < 8; ++i) s += adc_read();
    return (uint16_t)(s / 8u);
}

float read_macro() {
    static float filt = 0.35f;
    const float raw = read_adc_avg(0) / 4095.0f;
    filt += 0.08f * (raw - filt);
    return clamp01(filt);
}
}

int main() {
    stdio_init_all();

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 1);

    adc_init();
    adc_gpio_init(ADC_MACRO_PIN);

    AudioOutputI2S out;
    io::Pads pads;
    synth::BytebeatEngine synth;
    drums::DrumEngine drums;
    master::Master master;
    synth::EngineParams params;

    out.init();
    pads.init();
    synth.init();
    drums.init();
    master.init();

    // arrancar en un par más musical, no en el patch inicial fijo
    synth.next_formula_pair();
    synth.next_formula_pair();

    absolute_time_t next_control = delayed_by_ms(get_absolute_time(), 5);

    bool prev_pressed[4] = {false, false, false, false};

    while (true) {
        if (absolute_time_diff_us(get_absolute_time(), next_control) <= 0) {
            next_control = delayed_by_ms(next_control, 5);
            pads.update();

            const auto& p1 = pads.get(0);
            const auto& p2 = pads.get(1);
            const auto& p3 = pads.get(2);
            const auto& p4 = pads.get(3);

            // Trigger derivado del flanco de pressed:
            // mucho más confiable que depender del trigger interno.
            const bool trig1 = (p1.pressed && !prev_pressed[0]);
            const bool trig2 = (p2.pressed && !prev_pressed[1]);
            const bool trig3 = (p3.pressed && !prev_pressed[2]);
            const bool trig4 = (p4.pressed && !prev_pressed[3]);

            prev_pressed[0] = p1.pressed;
            prev_pressed[1] = p2.pressed;
            prev_pressed[2] = p3.pressed;
            prev_pressed[3] = p4.pressed;

            if (trig1) {
                synth.next_formula_pair();
            }
            if (trig2) {
                drums.trigger_kick();
            }
            if (trig3) {
                drums.trigger_snare();
            }
            if (trig4) {
                drums.trigger_hat();
            }

            gpio_put(LED_PIN, trig1 || trig2 || trig3 || trig4 ||
                              p1.pressed || p2.pressed || p3.pressed || p4.pressed);
        }

        const auto& p1 = pads.get(0);
        const auto& p2 = pads.get(1);
        const auto& p3 = pads.get(2);
        const auto& p4 = pads.get(3);

        const float macro = read_macro();

        // Mantener la ruta sonora estable y evitar ADCs flotando.
        params.drone  = true;
        params.volume = 0.92f;
        params.morph  = macro;
        params.color  = 0.18f + 0.82f * macro;
        params.tape   = p1.pressure;

        float s = synth.render(params);
        float d = drums.render(params.color, p2.pressure, p3.pressure, p4.pressure);

        // sidechain simple desde kick
        const float duck = 1.0f - drums.kick_env() * 0.50f;
        s *= duck;

        const float mix = s + d * 0.92f;
        const int16_t sample = (int16_t)(master.process(mix, 0.90f) * 32767.0f);
        out.write(sample, sample);
    }

    return 0;
}
