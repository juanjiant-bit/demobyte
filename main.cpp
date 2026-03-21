#include <cstdio>
#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "hardware/pin_config.h"
#include "audio/audio_output_i2s.h"

int main() {
    stdio_init_all();
    sleep_ms(1200);

    gpio_init(PIN_ONBOARD_LED);
    gpio_set_dir(PIN_ONBOARD_LED, GPIO_OUT);
    gpio_put(PIN_ONBOARD_LED, 0);

    AudioOutputI2S audio;
    audio.init();

    printf("PCM5102A tone test start\n");
    printf("GP10=BCK GP11=LRCK GP12=DIN\n");
    printf("FMT low (I2S), XSMT high (unmute), SCK not connected\n");

    uint32_t phase = 0;
    bool led = false;
    uint32_t led_div = 0;

    while (true) {
        phase += 440;
        if (phase >= AudioOutputI2S::SAMPLE_RATE) {
            phase -= AudioOutputI2S::SAMPLE_RATE;
        }

        const int16_t s = (phase < (AudioOutputI2S::SAMPLE_RATE / 2)) ? 24000 : -24000;
        audio.write(s, s);

        if (++led_div >= 22050) {
            led_div = 0;
            led = !led;
            gpio_put(PIN_ONBOARD_LED, led);
        }
    }
}
