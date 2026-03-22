#include <cstdint>
#include <cmath>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "pcm5102_i2s.pio.h"

#include "io/adc_handler.h"
#include "io/cap_pad_handler.h"

namespace {
constexpr uint PIN_BCLK = 10;
constexpr uint PIN_LRCK = 11;
constexpr uint PIN_DIN  = 12;
constexpr uint32_t SAMPLE_RATE = 44100;
constexpr int16_t AUDIO_AMPLITUDE = 9500;

constexpr float POT_FREQ_MIN = 60.0f;
constexpr float POT_FREQ_MAX = 2000.0f;

// Pad indices in the existing 3x5 matrix:
// row0: REC PLAY SHIFT SNAP1 SNAP2
// row1: MUTE HAT  SNAP3 SNAP4 SNAP5
// row2: KICK SNARE SNAP6 SNAP7 SNAP8
constexpr uint8_t PAD_REC   = 0;
constexpr uint8_t PAD_PLAY  = 1;
constexpr uint8_t PAD_SHIFT = 2;
constexpr uint8_t PAD_SNAP1 = 3;
constexpr uint8_t PAD_SNAP2 = 4;
constexpr uint8_t PAD_MUTE  = 5;
constexpr uint8_t PAD_HAT   = 6;
constexpr uint8_t PAD_SNAP3 = 7;
constexpr uint8_t PAD_SNAP4 = 8;
constexpr uint8_t PAD_SNAP5 = 9;
constexpr uint8_t PAD_KICK  = 10;
constexpr uint8_t PAD_SNARE = 11;
constexpr uint8_t PAD_SNAP6 = 12;
constexpr uint8_t PAD_SNAP7 = 13;
constexpr uint8_t PAD_SNAP8 = 14;

volatile uint32_t g_phase_inc_left = 0;
volatile uint32_t g_phase_inc_right = 0;
volatile uint16_t g_pad_mask = 0;
volatile uint8_t  g_mode = 0;
volatile bool     g_controls_ready = false;

static inline uint32_t i2s_slot_from_s16(int16_t sample) {
    return (uint32_t)(uint16_t)sample << 16;
}

static inline void i2s_write_stereo_s16(PIO pio, uint sm, int16_t left, int16_t right) {
    pio_sm_put_blocking(pio, sm, i2s_slot_from_s16(left));
    pio_sm_put_blocking(pio, sm, i2s_slot_from_s16(right));
}

static inline uint32_t phase_inc_from_freq(float freq_hz) {
    if (freq_hz < 0.0f) freq_hz = 0.0f;
    const double scale = 4294967296.0 / (double)SAMPLE_RATE;
    return (uint32_t)(freq_hz * scale);
}

static inline float freq_from_pot(float pot01) {
    if (pot01 < 0.0f) pot01 = 0.0f;
    if (pot01 > 1.0f) pot01 = 1.0f;
    const float ratio = POT_FREQ_MAX / POT_FREQ_MIN;
    return POT_FREQ_MIN * powf(ratio, pot01);
}

static inline bool pad_pressed(uint16_t mask, uint8_t pad) {
    return ((mask >> pad) & 1u) != 0u;
}

void control_core_entry() {
    AdcHandler adc;
    CapPadHandler pads;

    adc.init();
    pads.init(CapPadHandler::Preset::DRY());
    sleep_ms(50);
    pads.calibrate();

    g_phase_inc_left = phase_inc_from_freq(220.0f);
    g_phase_inc_right = phase_inc_from_freq(220.0f);
    g_pad_mask = 0;
    g_mode = 0;
    g_controls_ready = true;

    while (true) {
        adc.poll();
        pads.scan();

        const float pot_freq = freq_from_pot(adc.get(0));
        float left_freq = pot_freq;
        float right_freq = pot_freq;
        uint8_t mode = 0;

        const uint16_t mask = pads.get_state();

        if (pad_pressed(mask, PAD_REC)) {
            left_freq = 110.0f; right_freq = 110.0f; mode = 1;
        } else if (pad_pressed(mask, PAD_PLAY)) {
            left_freq = 220.0f; right_freq = 220.0f; mode = 2;
        } else if (pad_pressed(mask, PAD_SHIFT)) {
            left_freq = pot_freq; right_freq = pot_freq * 2.0f; mode = 3;
        } else if (pad_pressed(mask, PAD_SNAP1)) {
            left_freq = 261.63f; right_freq = 261.63f; mode = 4;
        } else if (pad_pressed(mask, PAD_SNAP2)) {
            left_freq = 293.66f; right_freq = 293.66f; mode = 5;
        } else if (pad_pressed(mask, PAD_MUTE)) {
            left_freq = 329.63f; right_freq = 329.63f; mode = 6;
        } else if (pad_pressed(mask, PAD_HAT)) {
            left_freq = 349.23f; right_freq = 349.23f; mode = 7;
        } else if (pad_pressed(mask, PAD_SNAP3)) {
            left_freq = 392.00f; right_freq = 392.00f; mode = 8;
        } else if (pad_pressed(mask, PAD_SNAP4)) {
            left_freq = 440.00f; right_freq = 440.00f; mode = 9;
        } else if (pad_pressed(mask, PAD_SNAP5)) {
            left_freq = 493.88f; right_freq = 493.88f; mode = 10;
        } else if (pad_pressed(mask, PAD_KICK)) {
            left_freq = 523.25f; right_freq = 523.25f; mode = 11;
        } else if (pad_pressed(mask, PAD_SNARE)) {
            left_freq = 587.33f; right_freq = 587.33f; mode = 12;
        } else if (pad_pressed(mask, PAD_SNAP6)) {
            left_freq = 659.25f; right_freq = 659.25f; mode = 13;
        } else if (pad_pressed(mask, PAD_SNAP7)) {
            left_freq = 698.46f; right_freq = 698.46f; mode = 14;
        } else if (pad_pressed(mask, PAD_SNAP8)) {
            left_freq = 783.99f; right_freq = 783.99f; mode = 15;
        }

        if (right_freq > 4000.0f) right_freq = 4000.0f;

        g_phase_inc_left = phase_inc_from_freq(left_freq);
        g_phase_inc_right = phase_inc_from_freq(right_freq);
        g_pad_mask = mask;
        g_mode = mode;

        sleep_ms(1);
    }
}

} // namespace

int main() {
    stdio_init_all();
    sleep_ms(100);

    multicore_launch_core1(control_core_entry);

    PIO pio = pio0;
    const uint sm = pio_claim_unused_sm(pio, true);
    const uint offset = pio_add_program(pio, &pcm5102_i2s_program);
    pcm5102_i2s_program_init(pio, sm, offset, PIN_DIN, PIN_BCLK, SAMPLE_RATE);

    for (int i = 0; i < 16; ++i) {
        i2s_write_stereo_s16(pio, sm, 0, 0);
    }

    uint32_t phase_left = 0;
    uint32_t phase_right = 0;

    while (!g_controls_ready) {
        i2s_write_stereo_s16(pio, sm, 0, 0);
    }

    while (true) {
        const uint32_t inc_l = g_phase_inc_left;
        const uint32_t inc_r = g_phase_inc_right;
        const uint8_t mode = g_mode;
        const uint16_t pad_mask = g_pad_mask;

        phase_left += inc_l;
        phase_right += inc_r;

        int16_t sample_l = (phase_left & 0x80000000u) ? AUDIO_AMPLITUDE : (int16_t)-AUDIO_AMPLITUDE;
        int16_t sample_r = (phase_right & 0x80000000u) ? AUDIO_AMPLITUDE : (int16_t)-AUDIO_AMPLITUDE;

        if (mode == 3) {
            sample_r = (int16_t)(sample_r / 2);
        }
        if (pad_pressed(pad_mask, PAD_MUTE)) {
            sample_l = (int16_t)(sample_l * 0.7f);
            sample_r = (int16_t)(sample_r * 0.7f);
        }

        i2s_write_stereo_s16(pio, sm, sample_l, sample_r);
    }
}
