#include <cstdio>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"

#include "hardware/pin_config.h"
#include "audio/audio_output_i2s.h"
#include "io/adc_handler.h"
#include "io/cap_pad_hybrid.h"
#include "synth/simple_synth.h"

int main() {
    stdio_init_all();
    sleep_ms(1200);

    gpio_init(PIN_ONBOARD_LED);
    gpio_set_dir(PIN_ONBOARD_LED, GPIO_OUT);
    gpio_put(PIN_ONBOARD_LED, 0);

    AudioOutputI2S audio;
    audio.init();

    AdcHandler adc;
    adc.init();

    CapPadHybrid pads;
    CapPadHybrid::Config pad_cfg;
    pad_cfg.discharge_us = 150;
    pad_cfg.max_charge_us = 2500;
    pad_cfg.calib_samples = 96;
    pad_cfg.threshold_floor_us = 14.0f;
    pad_cfg.threshold_noise_mul = 5.5f;
    pads.init(pad_cfg);

    SimpleSynth synth;
    synth.init(static_cast<float>(AudioOutputI2S::SAMPLE_RATE));

    absolute_time_t last_control = get_absolute_time();
    absolute_time_t last_blink = get_absolute_time();
    absolute_time_t last_print = get_absolute_time();
    const absolute_time_t boot_time = get_absolute_time();
    bool led = false;
    float startup_phase = 0.0f;

    printf("simple validation v3 hybrid start\n");
    printf("I2S GP10/11/12 | POT GP26 | ROW GP5 | PADS GP8 GP9 GP13 GP14\n");
    printf("No tocar pads durante el arranque/calibracion.\n");
    printf("Deberias escuchar un tono corto de diagnostico durante ~1.8 s al boot.\n");

    while (true) {
        const absolute_time_t now = get_absolute_time();

        if (absolute_time_diff_us(last_control, now) >= 4000) {
            last_control = now;
            adc.poll();
            pads.scan();

            synth.set_pot(adc.get());
            synth.set_pad_state(
                pads.is_pressed(CapPadHybrid::SNAP),
                pads.is_pressed(CapPadHybrid::KICK),
                pads.is_pressed(CapPadHybrid::SNARE),
                pads.is_pressed(CapPadHybrid::HAT),
                pads.get_pressure(CapPadHybrid::SNAP),
                pads.get_pressure(CapPadHybrid::KICK),
                pads.get_pressure(CapPadHybrid::SNARE),
                pads.get_pressure(CapPadHybrid::HAT),
                pads.just_pressed(CapPadHybrid::SNAP),
                pads.just_pressed(CapPadHybrid::KICK),
                pads.just_pressed(CapPadHybrid::SNARE),
                pads.just_pressed(CapPadHybrid::HAT)
            );
        }

        const bool any_pad =
            pads.is_pressed(CapPadHybrid::SNAP) ||
            pads.is_pressed(CapPadHybrid::KICK) ||
            pads.is_pressed(CapPadHybrid::SNARE) ||
            pads.is_pressed(CapPadHybrid::HAT);

        const int blink_us = any_pad ? 50000 : 110000;
        if (absolute_time_diff_us(last_blink, now) >= blink_us) {
            last_blink = now;
            led = !led;
            gpio_put(PIN_ONBOARD_LED, led);
        }

        if (absolute_time_diff_us(last_print, now) >= 250000) {
            last_print = now;
            printf(
                "pot=%.3f adc=%u pads=%d%d%d%d delta/thr=[%lu/%lu,%lu/%lu,%lu/%lu,%lu/%lu] raw/base=[%lu/%lu,%lu/%lu,%lu/%lu,%lu/%lu]\n",
                adc.get(),
                adc.raw(),
                pads.is_pressed(CapPadHybrid::SNAP),
                pads.is_pressed(CapPadHybrid::KICK),
                pads.is_pressed(CapPadHybrid::SNARE),
                pads.is_pressed(CapPadHybrid::HAT),
                (unsigned long)pads.get_delta_us(CapPadHybrid::SNAP),
                (unsigned long)pads.get_threshold_us(CapPadHybrid::SNAP),
                (unsigned long)pads.get_delta_us(CapPadHybrid::KICK),
                (unsigned long)pads.get_threshold_us(CapPadHybrid::KICK),
                (unsigned long)pads.get_delta_us(CapPadHybrid::SNARE),
                (unsigned long)pads.get_threshold_us(CapPadHybrid::SNARE),
                (unsigned long)pads.get_delta_us(CapPadHybrid::HAT),
                (unsigned long)pads.get_threshold_us(CapPadHybrid::HAT),
                (unsigned long)pads.get_raw_us(CapPadHybrid::SNAP),
                (unsigned long)pads.get_baseline_us(CapPadHybrid::SNAP),
                (unsigned long)pads.get_raw_us(CapPadHybrid::KICK),
                (unsigned long)pads.get_baseline_us(CapPadHybrid::KICK),
                (unsigned long)pads.get_raw_us(CapPadHybrid::SNARE),
                (unsigned long)pads.get_baseline_us(CapPadHybrid::SNARE),
                (unsigned long)pads.get_raw_us(CapPadHybrid::HAT),
                (unsigned long)pads.get_baseline_us(CapPadHybrid::HAT)
            );
        }

        int16_t s = 0;

        // Tono de diagnóstico al arranque: confirma la ruta RP2040 -> PIO -> PCM5102A
        // incluso si los pads todavía no están bien calibrados.
        if (absolute_time_diff_us(boot_time, now) < 1800000) {
            startup_phase += 220.0f / static_cast<float>(AudioOutputI2S::SAMPLE_RATE);
            if (startup_phase >= 1.0f) startup_phase -= 1.0f;
            const float v = (startup_phase < 0.5f) ? 0.22f : -0.22f;
            s = static_cast<int16_t>(v * 32767.0f);
        } else {
            s = synth.render_sample();
        }

        audio.write(s, s);
    }
}
