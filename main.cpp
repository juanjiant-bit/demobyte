// PAD DIAGNOSTIC V3 — circuito correcto
//
// GP5 = ROW drive común + 100nF a GND
// GP8/GP9/GP13/GP14 = COL sense (uno por pad, 1MΩ en serie)
//
// Mide el rango real de cada pad con auto-range,
// emite tonos para comunicar los valores sin necesitar serial.
//
// SECUENCIA:
//   Beep largo → auto-range (NO TOCAR ~5s)
//   4 beeps ascendentes → listo
//   Loop: 4 tonos (pad0..3), separados por beep grave
//
// INTERPRETACIÓN:
//   Todos los tonos iguales → buena calibración
//   Tono sube al tocar → pad funciona (más C = más tiempo)
//   Tono no cambia → pad no conectado o resistencia muy alta
//   Tono MUY agudo siempre → timeout, discharge insuficiente

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
static void tone(float hz, uint32_t ms) {
    const uint32_t n = 44100 * ms / 1000;
    const double inc = hz / 44100.0;
    double ph = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        i2s_write(ph < 0.5 ? 9000 : -9000, ph < 0.5 ? 9000 : -9000);
        ph += inc; if (ph >= 1.0) ph -= 1.0;
    }
}
static void silence(uint32_t ms) {
    for (uint32_t i = 0; i < 44100 * ms / 1000; ++i) i2s_write(0, 0);
}

// Mide un pad: ROW sube, esperar hasta que COL sube
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

static uint32_t measure_avg(uint8_t c, uint32_t dis, uint32_t max, int n) {
    uint64_t s = 0;
    for (int i = 0; i < n; ++i) { s += measure(c, dis, max); sleep_us(dis/2 + 50); }
    return (uint32_t)(s / n);
}

static uint32_t g_dis  = 500;
static uint32_t g_max  = 10000;
static float    g_base[4] = {};

static void auto_range() {
    // Encontrar discharge que da lecturas no-timeout en todos los pads
    const uint32_t steps[] = {100, 250, 500, 1000, 2000, 5000, 10000};
    for (int s = 0; s < 7; ++s) {
        g_dis = steps[s];
        g_max = g_dis * 20;
        bool ok = true;
        for (uint8_t c = 0; c < 4; ++c) {
            if (measure_avg(c, g_dis, g_max, 5) >= g_max * 9/10) { ok = false; break; }
        }
        if (ok) break;
        tone(220.0f, 60); silence(40);  // tick por cada intento fallido
    }

    // Medir baseline
    for (uint8_t c = 0; c < 4; ++c) {
        g_base[c] = (float)measure_avg(c, g_dis, g_max, 40);
        printf("pad%u: dis=%luus base=%.0fus max=%luus\n",
               c, (unsigned long)g_dis,
               (double)g_base[c], (unsigned long)g_max);
        tone(440.0f + c * 110.0f, 100); silence(60);
    }
}

int main() {
    set_sys_clock_khz(125000, true);
    stdio_init_all();

    g_pio = pio0;
    g_sm  = pio_claim_unused_sm(g_pio, true);
    const uint off = pio_add_program(g_pio, &pcm5102_i2s_program);
    pcm5102_i2s_program_init(g_pio, g_sm, off, PIN_DIN, PIN_BCLK, 44100);
    silence(100);

    gpio_init(PIN_LED); gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_init(PIN_ROW); gpio_set_dir(PIN_ROW, GPIO_OUT);
    gpio_put(PIN_ROW, 0);
    for (uint8_t c = 0; c < 4; ++c) {
        gpio_init(PIN_COL[c]); gpio_set_dir(PIN_COL[c], GPIO_IN);
        gpio_disable_pulls(PIN_COL[c]);
    }

    // Beep de inicio
    tone(300.0f, 500); silence(300);

    // Auto-range (no tocar)
    auto_range();

    // Escala de listo
    float notes[] = {261.63f, 329.63f, 392.0f, 523.25f};
    for (int i = 0; i < 4; ++i) { tone(notes[i], 120); silence(60); }
    silence(400);

    // Loop diagnóstico
    uint32_t cycle = 0;
    while (true) {
        gpio_put(PIN_LED, (cycle++ & 1));

        for (uint8_t c = 0; c < 4; ++c) {
            const uint32_t raw   = measure_avg(c, g_dis, g_max, 4);
            const float    delta = (float)raw - g_base[c];

            // 440Hz = baseline. Sube con dedo (más C = más tiempo = delta+)
            float freq = 440.0f + (delta / g_base[c]) * 600.0f;
            if (freq <  80.0f) freq =  80.0f;
            if (freq > 1400.0f) freq = 1400.0f;

            tone(freq, 180); silence(80);

            printf("p%u raw=%lu base=%.0f delta=%+.0f → %.0fHz %s\n",
                   c, (unsigned long)raw, (double)g_base[c],
                   (double)delta, (double)freq,
                   delta > g_base[c]*0.15f ? "TOUCH" : ".");
        }

        tone(80.0f, 100); silence(600);
    }
}
