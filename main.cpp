// main.cpp — BYT3 DAC / I2S VALIDATE MIN
// ─────────────────────────────────────────────────────────────────────────────
// Firmware mínimo para validar SOLO salida de audio por I2S.
//
// LO QUE HACE:
//   Core0 — emite un bytebeat fijo, fuerte y constante al PCM5102A
//   Core1 — LED heartbeat + lectura opcional del pote (solo debug visual)
//
// OBJETIVO:
//   Confirmar rápidamente que la cadena física está viva:
//   RP2040 → I2S (GP10/11/12) → PCM5102A → amp/parlante/auriculares activos
//
// PINOUT USADO:
//   GP10 → PCM5102 BCK
//   GP11 → PCM5102 LCK / LRCK
//   GP12 → PCM5102 DIN
//   GP25 → LED onboard
//   GP26 → pote opcional (solo para cambiar el patrón de blink)
//
// NOTA:
//   Este test NO usa pads. No toca cap sense, no genera patches, no usa graph.
//   Está pensado para reemplazar temporalmente el validate anterior y aislar
//   exclusivamente el chequeo del DAC / I2S.
// ─────────────────────────────────────────────────────────────────────────────

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"

#include "audio/audio_output_i2s.h"
#include "io/pot_handler.h"

static constexpr uint32_t SAMPLE_RATE = 44100;
static constexpr int32_t  GAIN_NUM    = 7;   // 7/4 = 1.75x
static constexpr int32_t  GAIN_DEN    = 4;

static AudioOutputI2S g_audio_out;
static PotHandler     g_pot;

static volatile bool  g_audio_ok     = false;
static volatile float g_pot_value    = 0.0f;
static volatile bool  g_fast_blink   = false;

static inline int16_t clamp_i16(int32_t x) {
    if (x >  32767) return  32767;
    if (x < -32768) return -32768;
    return (int16_t)x;
}

// Bytebeat fijo, deliberadamente obvio y agresivo.
// Base = cuadrada clara; extra = color digital para que no suene demasiado puro.
static inline uint8_t fixed_bytebeat(uint32_t t) {
    uint8_t sq1 = (t & 32u) ? 255u : 0u;      // ~689 Hz a 44.1 kHz
    uint8_t sq2 = (t & 128u) ? 96u  : 0u;     // sub grave / refuerzo
    uint8_t grit = ((t >> 3) ^ (t >> 5)) & 31u;
    return (uint8_t)(sq1 ^ sq2 ^ grit);
}

// ─────────────────────────────────────────────────────────────────────────────
// CORE 0 — Audio engine
// ─────────────────────────────────────────────────────────────────────────────
static void audio_core0_main() {
    g_audio_out.init();
    g_audio_ok = true;

    uint32_t t = 0;

    while (true) {
        for (uint16_t i = 0; i < 64; ++i) {
            uint8_t b = fixed_bytebeat(t++);

            // 0..255 → audio firmado centrado en 0
            int32_t s = ((int32_t)b - 128) << 8;

            // Ganancia alta con saturación dura controlada
            s = (s * GAIN_NUM) / GAIN_DEN;
            int16_t out = clamp_i16(s);

            // Mono duplicado a L/R
            g_audio_out.write(out, out);
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
    // Igual que tu validate anterior: clock alto para no introducir dudas.
    set_sys_clock_khz(200000, true);
    stdio_init_all();

    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);
    gpio_put(25, 0);

    multicore_launch_core1(input_core1_main);
    audio_core0_main();

    return 0;
}
