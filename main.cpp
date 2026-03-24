// PAD DIAGNOSTIC V2 — auto-range
//
// Primero mide el rango real de cada pad (min/max con y sin dedo)
// usando múltiples discharge times para encontrar el correcto.
// Después emite tonos para comunicar los valores.
//
// SECUENCIA AL ARRANCAR:
//   1. Beep largo (1s) → empezando auto-range scan
//   2. 4 beeps cortos → midiendo baseline sin dedo (NO TOCAR)
//   3. Beep largo → listo, entrando en loop de diagnóstico
//
// LOOP (cada 1.5s):
//   - 4 tonos: uno por pad, pitch = tiempo de carga mapeado
//   - Tono grave (<300Hz) = pad timeout o muy lento
//   - Tono medio (300-600Hz) = rango normal
//   - Tono agudo (>600Hz) = pad muy rápido
//   - Tono MUY agudo (>900Hz) = prácticamente instantáneo (posible CC)
//
// INSTRUCCIONES:
//   Escuchá el loop, después ponés el dedo en un pad
//   y me decís si el tono correspondiente cambia y en qué dirección.

#include <cstdint>
#include <cstdio>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/timer.h"
#include "hardware/clocks.h"
#include "pcm5102_i2s.pio.h"

constexpr uint PIN_ROW    = 5;
constexpr uint PIN_COL[4] = {8, 9, 13, 14};
constexpr uint PIN_LED    = 25;
constexpr uint PIN_BCLK   = 10;
constexpr uint PIN_DIN    = 12;

static PIO  g_pio;
static uint g_sm;

static inline void i2s_write(int16_t l, int16_t r) {
    pio_sm_put_blocking(g_pio, g_sm, (uint32_t)(uint16_t)l << 16);
    pio_sm_put_blocking(g_pio, g_sm, (uint32_t)(uint16_t)r << 16);
}

static void play_tone(float hz, uint32_t ms) {
    if (hz < 1.0f) hz = 1.0f;
    const uint32_t n = 44100 * ms / 1000;
    const double inc = hz / 44100.0;
    double ph = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        int16_t s = ph < 0.5 ? 10000 : -10000;
        i2s_write(s, s);
        ph += inc; if (ph >= 1.0) ph -= 1.0;
    }
}

static void silence(uint32_t ms) {
    const uint32_t n = 44100 * ms / 1000;
    for (uint32_t i = 0; i < n; ++i) i2s_write(0, 0);
}

// Mide tiempo de carga con discharge configurable
// Retorna µs, o max_us si timeout
static uint32_t measure(uint8_t c, uint32_t dis_us, uint32_t max_us) {
    gpio_put(PIN_ROW, 0);
    sleep_us(dis_us);
    const uint32_t t0 = time_us_32();
    gpio_put(PIN_ROW, 1);
    while (!gpio_get(PIN_COL[c])) {
        if ((time_us_32() - t0) >= max_us) {
            gpio_put(PIN_ROW, 0);
            return max_us;
        }
    }
    const uint32_t dt = time_us_32() - t0;
    gpio_put(PIN_ROW, 0);
    return dt;
}

// Promedio de N medidas
static uint32_t measure_avg(uint8_t c, uint32_t dis_us, uint32_t max_us, int n) {
    uint64_t sum = 0;
    for (int i = 0; i < n; ++i) {
        sum += measure(c, dis_us, max_us);
        sleep_us(dis_us + 50);
    }
    return (uint32_t)(sum / n);
}

static uint32_t g_baseline[4]    = {};
static uint32_t g_dis_us[4]      = {};  // discharge óptimo encontrado para cada pad
static uint32_t g_max_us[4]      = {};

// Auto-range: encuentra el discharge_us correcto para cada pad
// Prueba discharge times crecientes hasta que la lectura no sea timeout
static void auto_range() {
    const uint32_t discharge_steps[] = {50, 100, 200, 500, 1000, 2000, 5000};
    const int n_steps = 7;

    for (uint8_t c = 0; c < 4; ++c) {
        gpio_put(PIN_LED, (c & 1) ? 1 : 0);

        uint32_t best_dis = 5000;
        uint32_t best_val = 50000;

        for (int s = 0; s < n_steps; ++s) {
            const uint32_t dis = discharge_steps[s];
            const uint32_t max = dis * 20;  // max = 20x discharge
            const uint32_t val = measure_avg(c, dis, max, 10);

            // Si la lectura es < 90% del timeout, este discharge funciona
            if (val < (max * 9 / 10)) {
                best_dis = dis;
                best_val = val;
                break;
            }
        }

        g_dis_us[c]   = best_dis;
        g_max_us[c]   = best_dis * 20;
        g_baseline[c] = measure_avg(c, best_dis, g_max_us[c], 30);

        // Beep corto por pad completado
        play_tone(440.0f + c * 110.0f, 80);
        silence(50);
    }
}

int main() {
    set_sys_clock_khz(125000, true);
    stdio_init_all();

    g_pio = pio0;
    const uint off = pio_add_program(g_pio, &pcm5102_i2s_program);
    g_sm = pio_claim_unused_sm(g_pio, true);
    pcm5102_i2s_program_init(g_pio, g_sm, off, PIN_DIN, PIN_BCLK, 44100);
    silence(100);

    gpio_init(PIN_LED); gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_init(PIN_ROW); gpio_set_dir(PIN_ROW, GPIO_OUT); gpio_put(PIN_ROW, 0);
    for (uint8_t c = 0; c < 4; ++c) {
        gpio_init(PIN_COL[c]); gpio_set_dir(PIN_COL[c], GPIO_IN);
        gpio_disable_pulls(PIN_COL[c]);
    }

    // Beep de inicio
    play_tone(220.0f, 600);
    silence(200);

    // Auto-range (no tocar pads durante esto)
    auto_range();

    // Beep de listo: escala ascendente
    for (int i = 0; i < 4; ++i) {
        play_tone(261.63f * (1.0f + i * 0.25f), 100);
        silence(50);
    }
    silence(300);

    // Imprimir baselines por serial también
    printf("=== PAD BASELINES ===\n");
    for (uint8_t c = 0; c < 4; ++c) {
        printf("pad%u (GP%u): discharge=%luus baseline=%luus max=%luus\n",
               c, PIN_COL[c],
               (unsigned long)g_dis_us[c],
               (unsigned long)g_baseline[c],
               (unsigned long)g_max_us[c]);
    }

    // LOOP de diagnóstico
    while (true) {
        for (uint8_t c = 0; c < 4; ++c) {
            gpio_put(PIN_LED, 1);

            const uint32_t raw = measure_avg(c, g_dis_us[c], g_max_us[c], 5);
            const float base   = (float)g_baseline[c];
            const float delta  = (float)raw - base;
            const float range  = (float)g_max_us[c];

            // Encodear: el baseline suena a 440Hz
            // delta positivo (dedo) → tono sube
            // delta negativo (ruido) → tono baja
            // rango de ±50% del max_us = ±400Hz alrededor de 440Hz
            float freq = 440.0f + (delta / range) * 800.0f;
            if (freq < 80.0f)   freq = 80.0f;
            if (freq > 1400.0f) freq = 1400.0f;

            gpio_put(PIN_LED, 0);
            play_tone(freq, 180);
            silence(80);

            printf("pad%u raw=%lu base=%.0f delta=%.0f freq=%.0fHz\n",
                   c, (unsigned long)raw, (double)base,
                   (double)delta, (double)freq);
        }

        // Separador grave
        play_tone(80.0f, 120);
        silence(500);
    }
}
