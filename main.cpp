#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "audio/audio_output_i2s.h"

static AudioOutputI2S g_audio;

static constexpr uint32_t SAMPLE_RATE = 44100;
static constexpr int16_t AMP = 18000;

static constexpr uint16_t kSweepHz[] = {
    220, 330, 440, 550, 660, 880, 1100, 1320
};
static constexpr size_t kSweepCount = sizeof(kSweepHz) / sizeof(kSweepHz[0]);
static constexpr uint32_t STEP_MS = 350;

int main() {
    set_sys_clock_khz(153600, true);
    stdio_init_all();

    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);
    gpio_put(25, 0);

    g_audio.init();

    size_t tone_index = 0;
    uint32_t phase = 0;
    uint32_t samples_in_step = 0;
    uint32_t blink_div = 0;
    const uint32_t step_samples = (SAMPLE_RATE * STEP_MS) / 1000;

    while (true) {
        const uint32_t tone_hz = kSweepHz[tone_index];
        uint32_t period = SAMPLE_RATE / tone_hz;
        if (period < 2) period = 2;

        for (int i = 0; i < 256; ++i) {
            int16_t s = (phase < (period / 2)) ? AMP : -AMP;
            phase++;
            if (phase >= period) phase = 0;

            g_audio.write(s, s);
            samples_in_step++;

            if (samples_in_step >= step_samples) {
                samples_in_step = 0;
                tone_index = (tone_index + 1) % kSweepCount;
                phase = 0;
                break;
            }
        }

        blink_div++;
        if (blink_div >= 120) {
            blink_div = 0;
            gpio_xor_mask(1u << 25);
        }
    }

    return 0;
}
