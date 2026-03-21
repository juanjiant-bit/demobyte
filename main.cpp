#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "audio_i2s.pio.h"

static constexpr uint PIN_BCLK = 10;   // GP10 -> BCK
static constexpr uint PIN_LRCK = 11;   // GP11 -> LCK/LRCK
static constexpr uint PIN_DIN  = 12;   // GP12 -> DIN
static constexpr uint PIN_LED  = 25;

static constexpr uint32_t SAMPLE_RATE = 44100;
static constexpr int16_t AMP = 28000;
static constexpr uint32_t TONE_HZ = 440;

static inline void i2s_init_16b_stereo(PIO pio, uint sm, uint offset) {
    audio_i2s_program_init(pio, sm, offset, PIN_DIN, PIN_BCLK);

    // 32 BCLK por frame estéreo (16+16), 2 instrucciones por bit.
    const float div = (float)clock_get_hz(clk_sys) / (float)(SAMPLE_RATE * 32u * 2u);
    pio_sm_set_clkdiv(pio, sm, div);
    pio_sm_set_enabled(pio, sm, true);

    for (int i = 0; i < 16; ++i) {
        pio_sm_put_blocking(pio, sm, 0u);
    }
}

static inline void i2s_write_stereo(PIO pio, uint sm, int16_t left, int16_t right) {
    const uint32_t frame = ((uint32_t)(uint16_t)left << 16) | (uint16_t)right;
    pio_sm_put_blocking(pio, sm, frame);
}

int main() {
    set_sys_clock_khz(153600, true); // múltiplo limpio de 44.1k*32*2
    stdio_init_all();

    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);

    PIO pio = pio0;
    uint sm = 0;
    const uint offset = pio_add_program(pio, &audio_i2s_program);
    i2s_init_16b_stereo(pio, sm, offset);

    uint32_t phase = 0;
    const uint32_t period = SAMPLE_RATE / TONE_HZ;

    while (true) {
        for (int i = 0; i < 128; ++i) {
            const int16_t s = (phase < (period / 2)) ? AMP : -AMP;
            i2s_write_stereo(pio, sm, s, s);
            phase++;
            if (phase >= period) phase = 0;
        }
        gpio_xor_mask(1u << PIN_LED);
    }
}
