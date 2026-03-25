#include <cstdint>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pcm5102_i2s.pio.h"

namespace {
constexpr uint PIN_BCLK = 10;
constexpr uint PIN_LRCK = 11;
constexpr uint PIN_DIN  = 12;
constexpr uint SAMPLE_RATE = 44100;
constexpr int16_t TONE_AMPLITUDE = 12000;
constexpr uint32_t TOGGLE_PERIOD_SAMPLES = 200; // simple square wave

static inline uint32_t i2s_slot_from_s16(int16_t sample) {
    // 16-bit sample left-aligned into a standard 32-bit I2S slot.
    return (uint32_t)(uint16_t)sample << 16;
}

static inline void i2s_write_stereo_s16(PIO pio, uint sm, int16_t left, int16_t right) {
    // Philips-I2S-safe version: one 32-bit word per channel slot.
    pio_sm_put_blocking(pio, sm, i2s_slot_from_s16(left));
    pio_sm_put_blocking(pio, sm, i2s_slot_from_s16(right));
}
} // namespace

int main() {
    stdio_init_all();
    sleep_ms(50);

    PIO pio = pio0;
    const uint sm = pio_claim_unused_sm(pio, true);
    const uint offset = pio_add_program(pio, &pcm5102_i2s_program);

    pcm5102_i2s_program_init(pio, sm, offset, PIN_DIN, PIN_BCLK, SAMPLE_RATE);

    // Prime the DAC/PLL with silence.
    for (int i = 0; i < 16; ++i) {
        i2s_write_stereo_s16(pio, sm, 0, 0);
    }

    uint32_t counter = 0;
    while (true) {
        const int16_t sample = ((counter % TOGGLE_PERIOD_SAMPLES) < (TOGGLE_PERIOD_SAMPLES / 2))
                                   ? TONE_AMPLITUDE
                                   : (int16_t)-TONE_AMPLITUDE;

        i2s_write_stereo_s16(pio, sm, sample, sample);
        ++counter;
    }
}
