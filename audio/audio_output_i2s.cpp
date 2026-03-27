#include "audio/audio_output_i2s.h"

#include "hardware/pio.h"
#include "pcm5102_i2s.pio.h"
#include "pico/stdlib.h"

namespace audio {
namespace {
constexpr uint PIN_BCLK = 10;
constexpr uint PIN_LRCK = 11;
constexpr uint PIN_DIN  = 12;
constexpr uint32_t SAMPLE_RATE = 44100;

PIO g_pio = pio0;
uint g_sm = 0;
bool g_ready = false;

static inline void write_slot_blocking(PIO pio, uint sm, uint32_t slot_word) {
    pio_sm_put_blocking(pio, sm, slot_word);
}
}  // namespace

void AudioOutputI2S::init() {
    g_sm = pio_claim_unused_sm(g_pio, true);
    const uint offset = pio_add_program(g_pio, &pcm5102_i2s_program);
    pcm5102_i2s_program_init(g_pio, g_sm, offset, PIN_DIN, PIN_BCLK, SAMPLE_RATE);

    for (int i = 0; i < 16; ++i) {
        write(0, 0);
    }

    g_ready = true;
}

void AudioOutputI2S::write(int16_t left, int16_t right) {
    if (!g_ready) return;
    write_slot_blocking(g_pio, g_sm, slot_from_s16(left));
    write_slot_blocking(g_pio, g_sm, slot_from_s16(right));
}

}  // namespace audio
