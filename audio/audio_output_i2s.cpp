#include "audio_output_i2s.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "pcm5102_i2s.pio.h"

void AudioOutputI2S::init() {
    if (initialized_) return;

    offset_ = pio_add_program(pio_, &pcm5102_i2s_program);
    sm_     = pio_claim_unused_sm(pio_, true);

    pcm5102_i2s_program_init(pio_, sm_, offset_, PIN_DIN, PIN_BCLK, SAMPLE_RATE);

    // Prime the stream with bipolar zero so the DAC can lock to a valid I2S stream.
    for (int i = 0; i < 32; ++i) {
        pio_sm_put_blocking(pio_, sm_, 0u);
    }

    initialized_ = true;
}

void AudioOutputI2S::write(int16_t left, int16_t right) {
    // Standard Philips I2S, 32-bit slots per channel.
    // Left-align the 16-bit PCM sample into each 32-bit slot.
    const uint32_t l = ((uint32_t)(uint16_t)left) << 16;
    const uint32_t r = ((uint32_t)(uint16_t)right) << 16;
    pio_sm_put_blocking(pio_, sm_, l);
    pio_sm_put_blocking(pio_, sm_, r);
}
