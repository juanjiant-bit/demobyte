#include <cstdio>
#include <cmath>
#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "hardware/pin_config.h"
#include "audio/audio_output_i2s.h"
#include "io/adc_handler.h"

namespace {
constexpr float kToneHz = 440.0f;
constexpr float kTwoPi = 6.2831853071795864769f;
constexpr int16_t kMaxAmp = 30000;
}

int main() {
    stdio_init_all();
    sleep_ms(1200);

    gpio_init(PIN_ONBOARD_LED);
    gpio_set_dir(PIN_ONBOARD_LED, GPIO_OUT);
    gpio_put(PIN_ONBOARD_LED, 0);

    AdcHandler pot;
    pot.init();

    AudioOutputI2S audio;
    audio.init();

    printf("PCM5102A tone + gain test start\n");
    printf("GP10=BCK GP11=LRCK GP12=DIN GP26=POT\n");
    printf("Pot controls gain from near-zero to full-scale.\n");
    printf("FMT low (I2S), XSMT high (unmute), SCK not connected\n");

    float phase = 0.0f;
    bool led = false;
    uint32_t led_div = 0;
    uint32_t print_div = 0;

    while (true) {
        pot.poll();
        const float knob = pot.get();

        // Keep a tiny floor so the user can still hear that audio path is alive
        // even with the pot fully down.
        const float gain = 0.02f + (0.98f * knob);

        phase += kToneHz / static_cast<float>(AudioOutputI2S::SAMPLE_RATE);
        if (phase >= 1.0f) {
            phase -= 1.0f;
        }

        const float wave = std::sinf(phase * kTwoPi);
        int32_t sample = static_cast<int32_t>(wave * static_cast<float>(kMaxAmp) * gain);
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        const int16_t s = static_cast<int16_t>(sample);
        audio.write(s, s);

        if (++led_div >= 22050) {
            led_div = 0;
            led = !led;
            gpio_put(PIN_ONBOARD_LED, led);
        }

        if (++print_div >= 11025) {
            print_div = 0;
            printf("pot_raw=%u pot=%.3f gain=%.3f sample=%d\n", pot.raw(), knob, gain, static_cast<int>(s));
        }
    }
}
