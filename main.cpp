// main.cpp — BYT3 PAD DIAGNOSTIC V1
//
// Este firmware NO hace bytebeat. Solo diagnostica los pads.
//
// QUÉ ESCUCHÁS:
//   Cada segundo emite 4 tonos, uno por pad (GP8, GP9, GP13, GP14).
//   - Tono AGUDO (~800Hz)  = pad mide MUCHO tiempo de carga (>50µs)
//   - Tono GRAVE (~200Hz)  = pad mide POCO tiempo (<20µs)
//   - Tono MEDIO (~440Hz)  = rango normal (~20-50µs)
//   
//   Sin dedo: deberías escuchar 4 tonos similares (el baseline del pad vacío)
//   Con dedo: ese tono debería subir de tono (más tiempo de carga = más capacitancia)
//
//   El LED parpadea al ritmo de las mediciones (1 parpadeo por pad medido).
//
// PROCEDIMIENTO:
//   1. Escuchá los 4 tonos sin tocar nada → anotá si son graves, medios o agudos
//   2. Poné el dedo en GP8 (SNAP) → ¿sube el primer tono?
//   3. Poné el dedo en GP9 (KICK) → ¿sube el segundo tono?
//   4. etc.
//
// Mandame lo que escuchás: "4 tonos graves", "todos timeout", 
// "sube al tocar", "no cambia nada", etc.

#include <cstdint>
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

// Genera un tono de freq_hz durante duration_ms
static void play_tone(float freq_hz, uint32_t duration_ms) {
    if (freq_hz < 1.0f) freq_hz = 1.0f;
    const uint32_t samples = 44100 * duration_ms / 1000;
    const double inc = freq_hz / 44100.0;
    double phase = 0.0;
    for (uint32_t i = 0; i < samples; ++i) {
        int16_t s = (int16_t)((phase < 0.5) ? 10000 : -10000);
        i2s_write(s, s);
        phase += inc;
        if (phase >= 1.0) phase -= 1.0;
    }
}

static void play_silence(uint32_t duration_ms) {
    const uint32_t samples = 44100 * duration_ms / 1000;
    for (uint32_t i = 0; i < samples; ++i) i2s_write(0, 0);
}

// Mide el tiempo de carga de un pad probando múltiples discharge times
// Devuelve el tiempo raw en µs
static uint32_t measure_raw(uint8_t c, uint32_t discharge_us) {
    gpio_put(PIN_ROW, 0);
    sleep_us(discharge_us);
    const uint32_t t0 = time_us_32();
    gpio_put(PIN_ROW, 1);
    uint32_t limit = 2000;
    while (!gpio_get(PIN_COL[c])) {
        if ((time_us_32() - t0) >= limit) {
            gpio_put(PIN_ROW, 0);
            return limit;
        }
    }
    const uint32_t dt = time_us_32() - t0;
    gpio_put(PIN_ROW, 0);
    return dt;
}

int main() {
    set_sys_clock_khz(125000, true);

    // I2S init
    g_pio = pio0;
    const uint off = pio_add_program(g_pio, &pcm5102_i2s_program);
    g_sm = pio_claim_unused_sm(g_pio, true);
    pcm5102_i2s_program_init(g_pio, g_sm, off, PIN_DIN, PIN_BCLK, 44100);
    play_silence(100);

    // LED
    gpio_init(PIN_LED); gpio_set_dir(PIN_LED, GPIO_OUT);

    // GPIO pads
    gpio_init(PIN_ROW); gpio_set_dir(PIN_ROW, GPIO_OUT); gpio_put(PIN_ROW, 0);
    for (uint8_t c = 0; c < 4; ++c) {
        gpio_init(PIN_COL[c]); gpio_set_dir(PIN_COL[c], GPIO_IN);
        gpio_disable_pulls(PIN_COL[c]);
    }

    // Tono de inicio: 3 beeps rápidos
    for (int i = 0; i < 3; ++i) {
        play_tone(880.0f, 80);
        play_silence(80);
    }
    play_silence(300);

    // Loop: cada segundo mide los 4 pads y emite 4 tonos
    while (true) {
        for (uint8_t c = 0; c < 4; ++c) {
            gpio_put(PIN_LED, 1);

            // Probar con varios discharge times para encontrar el correcto
            // Usamos 500µs para garantizar descarga completa en cualquier escenario
            uint32_t raw = measure_raw(c, 500);

            gpio_put(PIN_LED, 0);

            // Mapear raw µs → frecuencia de tono
            // 0µs  → 100Hz  (timeout/cortocircuito)
            // 10µs → 200Hz  (pad muy rápido = poca capacitancia)  
            // 50µs → 440Hz  (rango medio con dedo)
            // 200µs→ 800Hz  (carga lenta)
            // 2000µs → 1200Hz (timeout = cap grande o circuito abierto)
            float freq;
            if (raw >= 2000) {
                freq = 1200.0f;  // timeout — circuito abierto o C muy grande
            } else {
                // Escala log: 10µs=200Hz, 100µs=600Hz, 1000µs=1000Hz
                freq = 100.0f + float(raw) * 0.55f;
            }

            play_tone(freq, 200);
            play_silence(100);
        }

        // Pausa entre ciclos + tono separador (50Hz muy grave)
        play_tone(80.0f, 100);
        play_silence(400);
    }
}
