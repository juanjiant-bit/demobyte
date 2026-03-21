// main.cpp — BYT3 DAC / I2S VALIDATE MIN
// ─────────────────────────────────────────────────────────────────────────────
// Firmware mínimo para validar SOLO salida de audio por I2S.
//
// LO QUE HACE:
//   Core0 — emite una cuadrada fija 1 kHz, muy obvia, al PCM5102A
//   Core1 — LED heartbeat + lectura opcional del pote (solo debug visual)
//
// OBJETIVO:
//   Confirmar rápidamente que la cadena física está viva:
//   RP2040 → I2S (GP10/11/12) → PCM5102A → monitores / amp
//
// PINOUT USADO:
//   GP10 → PCM5102 BCK
//   GP11 → PCM5102 LCK / LRCK
//   GP12 → PCM5102 DIN
//   GP25 → LED onboard
//   GP26 → pote opcional (solo para cambiar el patrón de blink)
// ─────────────────────────────────────────────────────────────────────────────

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"

#include "audio/audio_output_i2s.h"
#include "io/pot_handler.h"

static AudioOutputI2S g_audio_out;
static PotHandler     g_pot;

static volatile bool  g_audio_ok     = false;
static volatile float g_pot_value    = 0.0f;
static volatile bool  g_fast_blink   = false;

static constexpr uint32_t SAMPLE_RATE = 44100;
static constexpr uint32_t TONE_HZ     = 1000;
static constexpr int16_t  AMP         = 30000;

// ─────────────────────────────────────────────────────────────────────────────
// CORE 0 — Audio engine
// ─────────────────────────────────────────────────────────────────────────────
static void audio_core0_main() {
    g_audio_out.init();
    g_audio_ok = true;

    uint32_t phase = 0;
    const uint32_t half_period = SAMPLE_RATE / (TONE_HZ * 2u);

    while (true) {
        for (uint16_t i = 0; i < 256; ++i) {
            int16_t s = (phase < half_period) ? AMP : (int16_t)-AMP;
            phase++;
            if (phase >= (half_period * 2u)) {
                phase = 0;
            }

            g_audio_out.write(s, s);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// CORE 1 — LED + pot opcional
// ─────────────────────────────────────────────────────────────────────────────
static void input_core1_main() {
    g_pot.init();

    uint32_t counter = 0;

    while (true) {
        g_pot.poll();
        g_pot_value = g_pot.get();

        // Solo feedback visual: pote arriba = blink más rápido
        g_fast_blink = (g_pot_value > 0.55f);

        ++counter;
        uint32_t period = g_audio_ok ? (g_fast_blink ? 12u : 40u) : 6u;
        if (counter >= period) {
            counter = 0;
            gpio_xor_mask(1u << 25);
        }

        sleep_ms(10);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MAIN
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    set_sys_clock_khz(200000, true);
    stdio_init_all();

    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);
    gpio_put(25, 0);

    multicore_launch_core1(input_core1_main);
    audio_core0_main();

    return 0;
}
