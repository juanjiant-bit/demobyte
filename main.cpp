// main.cpp — BYT3 V4.0
//
// Motor bytebeat propio — sin envelope interna, sin AGC, sonido sostenido.
// Usa las 17 fórmulas exactas del stage11 BytebeatGraph.
// Agrega: filtro LP (pot controla cutoff), DC blocker, morph A↔B.
//
// PADS:
//   GP8  (pad 0) → nueva fórmula aleatoria (LED)
//   GP9  (pad 1) → kick drum
//   GP13 (pad 2) → snare
//   GP14 (pad 3) → hat + cambia zona (0=melódico→4=caos)
//
// POT: cutoff del filtro LP (grave→agudo)

#include <cstdint>
#include <cstdio>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/adc.h"
#include "hardware/timer.h"
#include "hardware/clocks.h"
#include "pcm5102_i2s.pio.h"

namespace {

constexpr uint PIN_ROW    = 5;
constexpr uint PIN_COL[4] = {8, 9, 13, 14};
constexpr uint PIN_LED    = 25;
constexpr uint PIN_POT    = 26;
constexpr uint PIN_BCLK   = 10;
constexpr uint PIN_DIN    = 12;

static PIO  g_pio;
static uint g_sm;
static inline void i2s_write(int16_t l, int16_t r) {
    pio_sm_put_blocking(g_pio, g_sm, (uint32_t)(uint16_t)l << 16);
    pio_sm_put_blocking(g_pio, g_sm, (uint32_t)(uint16_t)r << 16);
}

// ── Shared state ──────────────────────────────────────────────────
volatile float   g_pot       = 0.5f;
volatile uint8_t g_pad_event = 0;   // bits: 0=new_bb, 1=kick, 2=snare, 3=hat+zone
volatile bool    g_ready     = false;

// ── RNG ───────────────────────────────────────────────────────────
static uint32_t g_rng = 0xDEADBEEFu;
static inline uint32_t rng_next() {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return g_rng;
}
static inline uint8_t rng_u8() { return (uint8_t)(rng_next() >> 24); }

// ── Las 17 fórmulas del stage11 ───────────────────────────────────
static inline uint8_t formula(uint8_t id, uint32_t t, uint8_t seed) {
    const uint8_t s = seed;
    switch (id % 17u) {
    case 0:  return (uint8_t)(t * ((((t>>10)&42u) & 0xFFu) ? (((t>>10)&42u) & 0xFFu) : 1u));
    case 1:  return (uint8_t)(t * ((((t>>9)^(t>>11)) & 28u) + 4u));
    case 2:  return (uint8_t)(t * ((((t>>8)&15u)^((t>>11)&7u)) + 3u));
    case 3:  return (uint8_t)(t * ((((t>>10)&5u)|((t>>13)&2u)) + 2u));
    case 4:  return (uint8_t)(t & (t>>8));
    case 5:  return (uint8_t)(((t*5u)&(t>>7)) | ((t*3u)&(t>>10)));
    case 6:  return (uint8_t)(((t>>6)|(t*3u)) & ((t>>9)|(t*5u)));
    case 7:  return (uint8_t)(((t>>5)&(t>>8)) | ((t>>3)&(t*2u)));
    case 8:  return (uint8_t)(((t>>4)&(t>>7)) * ((255u-(t>>6))&255u));
    case 9:  return (uint8_t)(((t*(9u+(s&1u)))&(t>>4)) ^ ((t*(5u+((s>>1)&1u)))&(t>>7)));
    case 10: return (uint8_t)((t>>2)^(t>>5)^(t>>7));
    case 11: return (uint8_t)((t*((t>>9)&3u))&(t>>5));
    case 12: return (uint8_t)(t^(t>>3)^(t>>6));
    case 13: return (uint8_t)((t*(t>>9))^(t>>7)^(t>>13));
    case 14: return (uint8_t)(((t*7u)&(t>>9)) ^ ((t*11u)&(t>>11)));
    case 15: return (uint8_t)((t*((((t>>10)&21u)+3u))) | ((t>>7)&(t>>9)));
    case 16: return (uint8_t)(((t*((((t>>11)&13u)+2u))) ^ ((t*5u)&(t>>8))));
    default: return (uint8_t)t;
    }
}

// ── Motor bytebeat propio ─────────────────────────────────────────
struct BB {
    uint8_t  fa    = 2;    // fórmula A
    uint8_t  fb    = 10;   // fórmula B
    uint8_t  morph = 128;  // mezcla A↔B (0=100%A, 255=100%B)
    uint16_t rate  = 1;    // divisor de t (1=normal, 2=mitad de velocidad)
    uint8_t  seed  = 0;
    int32_t  lp    = 0;    // LP state
    int32_t  dcx   = 0;    // DC blocker
    int32_t  dcy   = 0;

    // Crossfade hacia nueva fórmula
    uint8_t  fa_prev = 2, fb_prev = 10;
    int32_t  xfade   = 256;  // 256=100% new, 0=100% prev (xfade en progreso si <256)

    void randomize(uint8_t zone) {
        fa_prev = fa; fb_prev = fb;
        // Zona 0-1: fórmulas melódicas (0-3, 10, 11, 15)
        // Zona 2-3: neutrales (4-7, 12)
        // Zona 4:   caos (8,9,13,14,16)
        static const uint8_t melodic[]  = {0,1,2,3,10,11,15};
        static const uint8_t neutral[]  = {4,5,6,7,10,12};
        static const uint8_t chaos[]    = {8,9,13,14,16,5,7};
        const uint8_t* pool = (zone <= 1) ? melodic : (zone <= 3) ? neutral : chaos;
        const uint8_t  pool_sz = (zone <= 1) ? 7 : (zone <= 3) ? 6 : 7;
        fa   = pool[rng_next() % pool_sz];
        fb   = pool[rng_next() % pool_sz];
        morph = rng_u8();
        rate  = 1 + (rng_next() % 3u);  // 1, 2, o 3
        seed  = rng_u8();
        xfade = 0;  // empieza crossfade desde prev hacia new
    }

    int16_t next(uint32_t t, float cutoff) {
        const uint32_t ts = t / (rate ? rate : 1);

        // Evaluar fórmulas
        uint8_t va = formula(fa, ts, seed);
        uint8_t vb = formula(fb, ts ^ (seed * 0x55u), seed ^ 0xA5u);

        // Crossfade entre prev y new si está activo
        if (xfade < 256) {
            const uint8_t pa = formula(fa_prev, ts, seed);
            const uint8_t pb = formula(fb_prev, ts ^ (seed*0x55u), seed^0xA5u);
            const uint8_t prev_mix = (uint8_t)(((uint16_t)pa*(255u-morph)
                                               + (uint16_t)pb*morph) >> 8);
            const uint8_t new_mix  = (uint8_t)(((uint16_t)va*(255u-morph)
                                               + (uint16_t)vb*morph) >> 8);
            // xfade 0→256 over 2048 samples
            const uint8_t out_raw = (uint8_t)(((uint32_t)prev_mix*(256-xfade)
                                              + (uint32_t)new_mix*xfade) >> 8);
            xfade += 1; // full crossfade in 256 samples = 5.8ms
            va = out_raw; vb = out_raw; // bypass morph mix below
        }

        // Morph A↔B
        const uint8_t raw = (xfade >= 256)
            ? (uint8_t)(((uint16_t)va*(255u-morph) + (uint16_t)vb*morph) >> 8)
            : va;  // already mixed above

        // Centrar en 0 y escalar a 16-bit
        int16_t s = (int16_t)((int)(raw) - 128) << 8;

        // Filtro LP: cutoff 0..1 → coeff
        // cutoff=0.0 → coeff=0.02 (muy cerrado, muy grave)
        // cutoff=1.0 → coeff=0.95 (casi abierto, casi sin filtro)
        const float coeff = 0.02f + cutoff * 0.93f;
        lp += (int32_t)((float)((int32_t)s - lp) * coeff);
        s = (int16_t)(lp >> 0);

        // DC blocker
        const int32_t y = (int32_t)s - dcx + ((dcy * 252) >> 8);
        dcx = (int32_t)s;
        dcy = y;
        s = (int16_t)(y < -32767 ? -32767 : y > 32767 ? 32767 : y);

        return s;
    }
};

static BB g_bb;

// ── Pad sensing ───────────────────────────────────────────────────
constexpr uint32_t DISCHARGE_US = 50000;
constexpr uint32_t MAX_US       = 200000;

static float pad_base[4] = {};
static bool  pad_on[4]   = {};
static bool  pad_prev[4] = {};

static uint32_t measure_pad(uint8_t c) {
    gpio_put(PIN_ROW, 0);
    sleep_us(DISCHARGE_US);
    const uint32_t t0 = time_us_32();
    gpio_put(PIN_ROW, 1);
    while (!gpio_get(PIN_COL[c])) {
        if ((time_us_32() - t0) >= MAX_US) {
            gpio_put(PIN_ROW, 0);
            return MAX_US;
        }
    }
    const uint32_t dt = time_us_32() - t0;
    gpio_put(PIN_ROW, 0);
    return dt;
}

static void calibrate_pads() {
    gpio_put(PIN_ROW, 0); sleep_ms(100);
    for (uint8_t c = 0; c < 4; ++c) {
        uint64_t sum = 0;
        for (int s = 0; s < 20; ++s) sum += measure_pad(c);
        pad_base[c] = float(sum / 20);
        printf("pad%u base=%.0fus\n", c, (double)pad_base[c]);
    }
}

static void scan_pads() {
    for (uint8_t c = 0; c < 4; ++c) {
        pad_prev[c] = pad_on[c];
        const uint32_t raw = measure_pad(c);
        const float d = (float)raw - pad_base[c];
        const float on_th  = pad_base[c] * 0.15f;
        const float off_th = pad_base[c] * 0.08f;
        pad_on[c] = pad_on[c] ? (d >= off_th) : (d >= on_th);
        if (!pad_on[c]) pad_base[c] += 0.01f * ((float)raw - pad_base[c]);
    }
}

static inline bool just_pressed(uint8_t n) { return pad_on[n] && !pad_prev[n]; }

// ── Drums ─────────────────────────────────────────────────────────
static uint32_t kick_ph = 0, kick_env = 0;
static uint32_t s_rng = 0xABCDu, snare_env = 0;
static uint32_t hat_env = 0;
static int32_t  duck = 256;

static void trig_kick()  { kick_env  = 0xFFFFu; kick_ph = 0; duck = 48; }
static void trig_snare() { snare_env = 0xFFFFu; s_rng = rng_next(); if(duck>80) duck=80; }
static void trig_hat()   { hat_env   = 0x6000u; if(duck>160) duck=160; }

static inline int16_t proc_kick() {
    if (!kick_env) return 0;
    kick_ph += 0x9000000u - (kick_env << 9);
    int16_t s = (int16_t)((((kick_ph>>31)?28000:-28000)*(int32_t)kick_env)>>16);
    kick_env = kick_env>150u ? kick_env-150u : 0u;
    return s;
}
static inline int16_t proc_snare() {
    if (!snare_env) return 0;
    s_rng^=s_rng<<7; s_rng^=s_rng>>9; s_rng^=s_rng<<8;
    int16_t s = (int16_t)(((int32_t)(int16_t)s_rng*(int32_t)snare_env)>>16);
    snare_env = snare_env>100u ? snare_env-100u : 0u;
    return s;
}
static inline int16_t proc_hat() {
    if (!hat_env) return 0;
    uint32_t h = hat_env*0x9E3779B9u;
    int16_t s = (int16_t)(((int32_t)(int16_t)h*(int32_t)hat_env)>>16);
    hat_env = hat_env>600u ? hat_env-600u : 0u;
    return s;
}

// ── Core1 ─────────────────────────────────────────────────────────
void core1_entry() {
    gpio_init(PIN_LED); gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_init(PIN_ROW); gpio_set_dir(PIN_ROW, GPIO_OUT); gpio_put(PIN_ROW, 0);
    for (uint8_t c = 0; c < 4; ++c) {
        gpio_init(PIN_COL[c]); gpio_set_dir(PIN_COL[c], GPIO_IN);
        gpio_disable_pulls(PIN_COL[c]);
    }
    adc_init(); adc_gpio_init(PIN_POT); adc_select_input(0);

    gpio_put(PIN_LED, 1);
    calibrate_pads();
    gpio_put(PIN_LED, 0);
    g_ready = true;

    float pot_s = float(adc_read()) / 4095.0f;
    uint8_t zone = 0;  // zona 0 = melódico por default

    while (true) {
        pot_s += 0.12f * (float(adc_read())/4095.0f - pot_s);
        g_pot = pot_s;
        scan_pads();

        if (just_pressed(0)) { g_pad_event |= 1u; gpio_put(PIN_LED,1); }
        if (!pad_on[0])        gpio_put(PIN_LED, 0);
        if (just_pressed(1))   g_pad_event |= 2u;
        if (just_pressed(2))   g_pad_event |= 4u;
        if (just_pressed(3)) {
            zone = (uint8_t)((zone + 1u) % 5u);
            g_pad_event |= 8u;
        }
    }
}

} // namespace

// ── Core0: audio ─────────────────────────────────────────────────
int main() {
    set_sys_clock_khz(125000, true);
    stdio_init_all();

    g_pio = pio0;
    const uint off = pio_add_program(g_pio, &pcm5102_i2s_program);
    g_sm = pio_claim_unused_sm(g_pio, true);
    pcm5102_i2s_program_init(g_pio, g_sm, off, PIN_DIN, PIN_BCLK, 44100);
    for (int i = 0; i < 32; ++i) i2s_write(0, 0);

    multicore_launch_core1(core1_entry);
    while (!g_ready) i2s_write(0, 0);

    // Fórmula inicial conocida que suena bien
    g_bb.fa = 2; g_bb.fb = 10; g_bb.morph = 100;
    g_bb.rate = 1; g_bb.seed = 42;

    uint32_t t    = 0;
    uint8_t  zone = 0;
    uint32_t cr   = 0;

    while (true) {
        if (++cr >= 32u) {
            cr = 0;
            const uint8_t ev = g_pad_event;
            if (ev) {
                g_pad_event = 0;
                if (ev & 1u) g_bb.randomize(zone);
                if (ev & 2u) trig_kick();
                if (ev & 4u) trig_snare();
                if (ev & 8u) { zone=(uint8_t)((zone+1u)%5u); g_bb.randomize(zone); }
            }
            // Duck recovery
            duck += (256 - duck) >> 4;
            if (duck > 254) duck = 256;
        }

        // Bytebeat: pot controla el cutoff del LP
        int16_t synth = g_bb.next(t, g_pot);
        synth = (int16_t)((int32_t)synth * duck >> 8);

        int32_t out = (int32_t)synth
                    + (int32_t)proc_kick()
                    + (int32_t)proc_snare()
                    + (int32_t)proc_hat();
        if (out >  32767) out =  32767;
        if (out < -32768) out = -32768;

        i2s_write((int16_t)out, (int16_t)out);
        ++t;
    }
}
