// audio_output_i2s.cpp — Bytebeat Machine
// PCM5102A via Philips I2S + PIO0/SM0
#include "audio_output_i2s.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "pcm5102_i2s.pio.h"

namespace {
static inline uint32_t i2s_slot_from_s16(int16_t sample) {
    // 16-bit signed sample left-aligned in a standard 32-bit Philips-I2S slot.
    return (uint32_t)(uint16_t)sample << 16;
}
}

void AudioOutputI2S::init() {
    if (initialized_) return;

    // Usar la función de init del c-sdk block del .pio
    // PIO0, SM0 — hardcodeado, no conflict con WS2812 (que usa PIO1)
    offset_ = pio_add_program(pio_, &pcm5102_i2s_program);
    sm_     = pio_claim_unused_sm(pio_, true);

    pcm5102_i2s_program_init(pio_, sm_, offset_,
                              PIN_DIN, PIN_BCLK, SAMPLE_RATE);

    // Precargar silencio para que el PLL del PCM5102A se enganche
    // antes de que lleguen muestras reales (evita pops y ayuda a lockear).
    for (int i = 0; i < 16; ++i) {
        pio_sm_put_blocking(pio_, sm_, i2s_slot_from_s16(0));
        pio_sm_put_blocking(pio_, sm_, i2s_slot_from_s16(0));
    }

    initialized_ = true;
}

void AudioOutputI2S::write(int16_t left, int16_t right) {
    // Philips I2S seguro para PCM5102:
    // un word de 32 bits por slot de canal, con la muestra de 16 bits
    // alineada a la izquierda dentro del slot.
    pio_sm_put_blocking(pio_, sm_, i2s_slot_from_s16(left));
    pio_sm_put_blocking(pio_, sm_, i2s_slot_from_s16(right));
}
