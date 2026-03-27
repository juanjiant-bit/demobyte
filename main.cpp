
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "audio/audio_output_i2s.h"
#include "io/pads.h"
#include "synth/bytebeat_engine.h"
#include "drums/drum_engine.h"
#include "master/master.h"
#include <cmath>

using audio::AudioOutputI2S;

namespace {
constexpr uint LED_PIN = 25;
constexpr uint ADC_VOL_PIN   = 26; // ADC0
constexpr uint ADC_MORPH_PIN = 27; // ADC1
constexpr uint ADC_COLOR_PIN = 28; // ADC2

static inline float clamp01(float x) {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

struct Pots {
    float vol = 0.9f;
    float morph = 0.0f;
    float color = 0.45f;
};

uint16_t read_adc_avg(uint input) {
    adc_select_input(input);
    uint32_t s = 0;
    for (int i = 0; i < 8; ++i) s += adc_read();
    return (uint16_t)(s / 8u);
}

void update_pots(Pots& p) {
    static float f0 = 0.9f, f1 = 0.0f, f2 = 0.45f;

    const float r0 = read_adc_avg(0) / 4095.0f;
    const float r1 = read_adc_avg(1) / 4095.0f;
    const float r2 = read_adc_avg(2) / 4095.0f;

    f0 += 0.10f * (r0 - f0);
    f1 += 0.10f * (r1 - f1);
    f2 += 0.10f * (r2 - f2);

    p.vol   = clamp01(f0);
    p.morph = clamp01(f1);
    p.color = clamp01(f2);
}
} // namespace

int main() {
    stdio_init_all();

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 1);

    adc_init();
    adc_gpio_init(ADC_VOL_PIN);
    adc_gpio_init(ADC_MORPH_PIN);
    adc_gpio_init(ADC_COLOR_PIN);

    AudioOutputI2S out;
    io::Pads pads;
    synth::BytebeatEngine synth;
    drums::DrumEngine drums;
    master::Master master;
    Pots pots;
    synth::EngineParams params;

    out.init();
    pads.init();
    synth.init();
    drums.init();
    master.init();

    synth.next_formula_pair();
    synth.next_formula_pair();

    absolute_time_t next_control = delayed_by_ms(get_absolute_time(), 5);

    while (true) {
        if (absolute_time_diff_us(get_absolute_time(), next_control) <= 0) {
            next_control = delayed_by_ms(next_control, 5);

            pads.update();
            update_pots(pots);

            const auto& p1 = pads.get(0);
            const auto& p2 = pads.get(1);
            const auto& p3 = pads.get(2);
            const auto& p4 = pads.get(3);

            // Si en tu build bueno el trigger directo funciona, mantené esto.
            // Si no funciona, reemplazalo por edge detect sobre pressed.
            if (p1.trigger) synth.next_formula_pair();
            if (p2.trigger) drums.trigger_kick();
            if (p3.trigger) drums.trigger_snare();
            if (p4.trigger) drums.trigger_hat();

            gpio_put(LED_PIN,
                p1.trigger || p2.trigger || p3.trigger || p4.trigger ||
                p1.pressed || p2.pressed || p3.pressed || p4.pressed
            );
        }

        const auto& p1 = pads.get(0);
        const auto& p2 = pads.get(1);
        const auto& p3 = pads.get(2);
        const auto& p4 = pads.get(3);

        // Volumen igual que antes
        params.volume = 0.25f + pots.vol * 0.75f;

        // Rango perceptual mucho más grande para morph y color
        // sin salir de 0..1 hacia afuera.
        params.morph = powf(clamp01(pots.morph), 0.45f);
        params.color = powf(clamp01(pots.color), 0.35f);

        params.drone = true;
        params.tape  = p1.pressure;

        float s = synth.render(params);
        float d = drums.render(params.color, p2.pressure, p3.pressure, p4.pressure);

        const float duck = 1.0f - drums.kick_env() * 0.52f;
        s *= duck;

        float mix = s + d * 0.92f;
        int16_t sample = (int16_t)(master.process(mix, pots.vol) * 32767.0f);
        out.write(sample, sample);
    }

    return 0;
}
