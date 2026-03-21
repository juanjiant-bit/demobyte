#include "audio_output_i2s.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "pcm5102_i2s.pio.h"

void AudioOutputI2S::init() {
    if (initialized_) return;

    offset_ = pio_add_program(pio_, &pcm5102_i2s_program);
    sm_     = pio_claim_unused_sm(pio_, true);

    pcm5102_i2s_program_init(pio_, sm_, offset_,
                              PIN_DIN, PIN_BCLK, SAMPLE_RATE);

    for (int i = 0; i < 16; ++i) {
        pio_sm_put_blocking(pio_, sm_, 0u);
    }

    initialized_ = true;
}

void AudioOutputI2S::write(int16_t left, int16_t right) {
    const uint32_t frame =
        ((uint32_t)(uint16_t)left << 16) |
        ((uint32_t)(uint16_t)right);

    pio_sm_put_blocking(pio_, sm_, frame);
}
