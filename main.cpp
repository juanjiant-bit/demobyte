#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "audio/audio_output_i2s.h"

static AudioOutputI2S g_audio;

static constexpr uint32_t SAMPLE_RATE = 44100;
static constexpr int16_t AMP = 16000;

// Escalera ascendente clara
static const uint32_t kFreqs[] = {220, 330, 440, 550, 660, 880, 1100, 1320};
static constexpr int kNumFreqs = sizeof(kFreqs) / sizeof(kFreqs[0]);
static constexpr uint32_t STEP_MS = 350;

int main() {
    set_sys_clock_khz(153600, true);
    stdio_init_all();

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 0);

    g_audio.init();

    absolute_time_t next_step = make_timeout_time_ms(STEP_MS);
    int current_step = 0;
    uint32_t phase = 0;

    while (true) {
        if (absolute_time_diff_us(get_absolute_time(), next_step) <= 0) {
            next_step = make_timeout_time_ms(STEP_MS);
            current_step = (current_step + 1) % kNumFreqs;
            gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
        }

        const uint32_t freq = kFreqs[current_step];
        const uint32_t period = SAMPLE_RATE / freq;

        // Bloque de audio chico pero continuo
        for (int i = 0; i < 256; ++i) {
            int16_t s = (phase < (period / 2)) ? AMP : -AMP;
            phase++;
            if (phase >= period) phase = 0;

            g_audio.write(s, s);
        }
    }

    return 0;
}
