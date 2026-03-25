// Dem0!Byt3 V2 — bytebeat + drums sobre el patrón I2S que funciona
//
// Basado 1:1 en prueba_i2s_andando: mismo PIO, mismo init, mismo tight loop.
// Se agrega encima:
//   - 17 fórmulas bytebeat propias (sin BytebeatGraph, sin StateManager)
//   - DrumEngine real del firmware (kick/snare/hat con families)
//   - Pads capacitivos (circuito confirmado: GP5=ROW, GP8/9/13/14=COL, 1MΩ en COL)
//   - Pot GP26 → cutoff filtro LP del bytebeat
//
// PADS:
//   GP8  → randomiza fórmula bytebeat + LED
//   GP9  → kick
//   GP13 → snare
//   GP14 → hat
//
// Core0: audio tight loop (bytebeat + drums → I2S)
// Core1: pads + pot → volatile flags

#include <cstdint>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/adc.h"
#include "hardware/timer.h"
#include "pcm5102_i2s.pio.h"
#include "audio/drums/drum_engine.h"

// ── I2S (igual que prueba_i2s_andando) ───────────────────────────
static PIO  g_pio = pio0;
static uint g_sm  = 0;

static inline void i2s_write(int16_t l, int16_t r) {
    pio_sm_put_blocking(g_pio, g_sm,
        (uint32_t)(uint16_t)l << 16);
    pio_sm_put_blocking(g_pio, g_sm,
        (uint32_t)(uint16_t)r << 16);
}

// ── Pines ─────────────────────────────────────────────────────────
constexpr uint PIN_ROW    = 5;
constexpr uint PIN_COL[4] = {8, 9, 13, 14};
constexpr uint PIN_LED    = 25;
constexpr uint PIN_POT    = 26;
constexpr uint PIN_BCLK   = 10;
constexpr uint PIN_DIN    = 12;
constexpr uint SAMPLE_RATE = 44100;

// ── Shared state ──────────────────────────────────────────────────
static volatile float   g_pot       = 0.5f;
static volatile uint8_t g_pad_event = 0;  // bits: 0=new_bb, 1=kick, 2=snare, 3=hat
static volatile bool    g_ready     = false;

// ── RNG ───────────────────────────────────────────────────────────
static uint32_t g_rng = 0xDEADBEEFu;
static inline uint32_t rng_next() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

// ── Bytebeat — 17 fórmulas directas ──────────────────────────────
static inline uint8_t bb_formula(uint8_t id, uint32_t t, uint8_t seed) {
    const uint8_t s = seed;
    switch (id % 17u) {
    case 0:  return (uint8_t)(t*((((t>>10)&42u)&0xFFu)?(((t>>10)&42u)&0xFFu):1u));
    case 1:  return (uint8_t)(t*((((t>>9)^(t>>11))&28u)+4u));
    case 2:  return (uint8_t)(t*((((t>>8)&15u)^((t>>11)&7u))+3u));
    case 3:  return (uint8_t)(t*((((t>>10)&5u)|((t>>13)&2u))+2u));
    case 4:  return (uint8_t)(t&(t>>8));
    case 5:  return (uint8_t)(((t*5u)&(t>>7))|((t*3u)&(t>>10)));
    case 6:  return (uint8_t)(((t>>6)|(t*3u))&((t>>9)|(t*5u)));
    case 7:  return (uint8_t)(((t>>5)&(t>>8))|((t>>3)&(t*2u)));
    case 8:  return (uint8_t)(((t>>4)&(t>>7))*((255u-(t>>6))&255u));
    case 9:  return (uint8_t)(((t*(9u+(s&1u)))&(t>>4))^((t*(5u+((s>>1)&1u)))&(t>>7)));
    case 10: return (uint8_t)((t>>2)^(t>>5)^(t>>7));
    case 11: return (uint8_t)((t*((t>>9)&3u))&(t>>5));
    case 12: return (uint8_t)(t^(t>>3)^(t>>6));
    case 13: return (uint8_t)((t*(t>>9))^(t>>7)^(t>>13));
    case 14: return (uint8_t)(((t*7u)&(t>>9))^((t*11u)&(t>>11)));
    case 15: return (uint8_t)((t*((((t>>10)&21u)+3u)))|((t>>7)&(t>>9)));
    case 16: return (uint8_t)(((t*((((t>>11)&13u)+2u)))^((t*5u)&(t>>8))));
    default: return (uint8_t)t;
    }
}

// ── Motor bytebeat propio ─────────────────────────────────────────
struct BB {
    uint8_t  fa = 2, fb = 10, morph = 128, seed = 0;
    uint16_t rate = 1;
    int32_t  lp = 0, dcx = 0, dcy = 0;

    // crossfade al randomizar
    uint8_t  fa_prev = 2, fb_prev = 10, morph_prev = 128;
    uint16_t xfade = 256;  // 256=nuevo completo

    void randomize() {
        fa_prev = fa; fb_prev = fb; morph_prev = morph;
        fa    = (uint8_t)(rng_next() % 17u);
        fb    = (uint8_t)(rng_next() % 17u);
        morph = (uint8_t)(rng_next() >> 24);
        rate  = (uint16_t)(1u + rng_next() % 3u);
        seed  = (uint8_t)(rng_next() >> 24);
        xfade = 0;
    }

    int16_t next(uint32_t t, float cutoff) {
        const uint32_t ts = t / (rate ? rate : 1u);

        uint8_t va = bb_formula(fa, ts, seed);
        uint8_t vb = bb_formula(fb, ts ^ (uint32_t)(seed * 0x55u),
                                 seed ^ 0xA5u);
        uint8_t raw;

        if (xfade < 256) {
            // crossfade suave desde fórmulas anteriores
            uint8_t pa = bb_formula(fa_prev, ts, seed);
            uint8_t pb = bb_formula(fb_prev, ts ^ (uint32_t)(seed*0x55u),
                                    seed ^ 0xA5u);
            const uint8_t old_mix = (uint8_t)(((uint16_t)pa*(255u-morph_prev)
                                               +(uint16_t)pb*morph_prev)>>8);
            const uint8_t new_mix = (uint8_t)(((uint16_t)va*(255u-morph)
                                               +(uint16_t)vb*morph)>>8);
            raw = (uint8_t)(((uint32_t)old_mix*(256u-xfade)
                             +(uint32_t)new_mix*xfade)>>8);
            ++xfade;
        } else {
            raw = (uint8_t)(((uint16_t)va*(255u-morph)
                             +(uint16_t)vb*morph)>>8);
        }

        // centrar y escalar a 16-bit
        int32_t s = ((int32_t)(int8_t)(raw ^ 0x80u)) << 8;

        // filtro LP (cutoff 0→1 = grave→abierto)
        const float c = 0.02f + cutoff * 0.93f;
        lp += (int32_t)((float)((int32_t)s - lp) * c);
        s = lp;

        // DC blocker
        const int32_t y = s - dcx + ((dcy * 252) >> 8);
        dcx = s; dcy = y;
        s = (y < -32767) ? -32767 : (y > 32767) ? 32767 : y;

        return (int16_t)s;
    }
};

// ── Pad sensing ───────────────────────────────────────────────────
static float    pad_base[4] = {};
static bool     pad_on[4]   = {};
static bool     pad_prev[4] = {};

static uint32_t measure_pad(uint8_t c) {
    gpio_put(PIN_ROW, 0);
    sleep_us(20000);  // 20ms discharge — más tiempo = descarga completa en pads grandes
    const uint32_t t0 = time_us_32();
    gpio_put(PIN_ROW, 1);
    while (!gpio_get(PIN_COL[c])) {
        if ((time_us_32()-t0) >= 30000u) {
            gpio_put(PIN_ROW, 0); return 30000u;
        }
    }
    const uint32_t dt = time_us_32()-t0;
    gpio_put(PIN_ROW, 0);
    return dt;
}

static void calibrate_pads() {
    gpio_put(PIN_ROW, 0); sleep_ms(50);
    for (uint8_t c = 0; c < 4; ++c) {
        uint64_t sum = 0;
        for (int s = 0; s < 10; ++s) sum += measure_pad(c);
        pad_base[c] = float(sum / 10);
    }
}

static void scan_pads() {
    for (uint8_t c = 0; c < 4; ++c) {
        pad_prev[c] = pad_on[c];
        const uint32_t raw = measure_pad(c);
        const float d = float(raw) - pad_base[c];
        const float on_th  = pad_base[c] * 0.35f;  // 35% — requiere toque firme
        const float off_th = pad_base[c] * 0.20f;  // 20% — hyst para evitar chatter
        pad_on[c] = pad_on[c] ? (d >= off_th) : (d >= on_th);
        if (!pad_on[c])
            pad_base[c] += 0.002f * (float(raw) - pad_base[c]);  // drift lento = más estable
    }
}

// ── Core1: pads + pot ─────────────────────────────────────────────
static void core1_main() {
    gpio_init(PIN_LED); gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_init(PIN_ROW); gpio_set_dir(PIN_ROW, GPIO_OUT);
    gpio_put(PIN_ROW, 0);
    for (uint8_t c = 0; c < 4; ++c) {
        gpio_init(PIN_COL[c]);
        gpio_set_dir(PIN_COL[c], GPIO_IN);
        gpio_disable_pulls(PIN_COL[c]);
    }
    adc_init(); adc_gpio_init(PIN_POT); adc_select_input(0);

    gpio_put(PIN_LED, 1);
    calibrate_pads();
    gpio_put(PIN_LED, 0);
    g_ready = true;

    float pot_s = float(adc_read()) / 4095.0f;
    while (true) {
        pot_s += 0.12f * (float(adc_read()) / 4095.0f - pot_s);
        g_pot = pot_s;
        scan_pads();
        if (pad_on[0] && !pad_prev[0]) { g_pad_event |= 1u; gpio_put(PIN_LED,1); }
        if (!pad_on[0]) gpio_put(PIN_LED, 0);
        if (pad_on[1] && !pad_prev[1]) g_pad_event |= 2u;
        if (pad_on[2] && !pad_prev[2]) g_pad_event |= 4u;
        if (pad_on[3] && !pad_prev[3]) g_pad_event |= 8u;
    }
}

// ── Core0: audio ──────────────────────────────────────────────────
int main() {
    // NO set_sys_clock_khz — igual que prueba_i2s_andando
    // El PIO calcula su divisor con el clock por defecto (125MHz)
    // Si se cambia el sys clock DESPUÉS del init del PIO, el audio se desafina

    // Init I2S — igual que prueba_i2s_andando
    const uint off = pio_add_program(g_pio, &pcm5102_i2s_program);
    g_sm = pio_claim_unused_sm(g_pio, true);
    pcm5102_i2s_program_init(g_pio, g_sm, off, PIN_DIN, PIN_BCLK, SAMPLE_RATE);

    // Prime DAC con silencio
    for (int i = 0; i < 16; ++i) i2s_write(0, 0);

    // Init drum engine
    static DrumEngine drums;
    drums.init();
    drums.set_params(0.3f, 0.5f, 0.7f);  // color=0.3, decay=0.5, duck=0.7

    // Init bytebeat
    static BB bb;

    // Arrancar Core1
    multicore_launch_core1(core1_main);
    while (!g_ready) sleep_ms(10);

    uint32_t t   = 0;
    uint32_t cr  = 0;

    while (true) {
        // Control block cada 32 samples — leer eventos de pad
        if (++cr >= 32u) {
            cr = 0;
            const uint8_t ev = g_pad_event;
            if (ev) {
                g_pad_event = 0;
                if (ev & 1u) bb.randomize();
                if (ev & 2u) drums.trigger(DRUM_KICK);
                if (ev & 4u) drums.trigger(DRUM_SNARE);
                if (ev & 8u) drums.trigger(DRUM_HAT);
            }
            // Actualizar parámetros de drums con el pot
            const float pot = g_pot;
            drums.set_params(pot, pot * 0.8f + 0.1f, -1.0f);
        }

        // Bytebeat sample
        const int16_t bb_s = bb.next(t, g_pot);

        // Drum sample
        int16_t drum_l = 0, drum_r = 0;
        int32_t sidechain_q15 = 32767;  // sin duck por defecto
        drums.process(drum_l, drum_r, sidechain_q15);

        // Sidechain del bytebeat: cuando el kick golpea, baja el bytebeat
        // sidechain_q15: 32767=sin duck, 0=mute total
        const int32_t bb_ducked = ((int32_t)bb_s * sidechain_q15) >> 15;

        // Mezcla: bytebeat + drums
        int32_t out_l = bb_ducked + (int32_t)drum_l;
        int32_t out_r = bb_ducked + (int32_t)drum_r;

        // Clamp
        if (out_l >  32767) out_l =  32767;
        if (out_l < -32768) out_l = -32768;
        if (out_r >  32767) out_r =  32767;
        if (out_r < -32768) out_r = -32768;

        i2s_write((int16_t)out_l, (int16_t)out_r);
        ++t;
    }
}
