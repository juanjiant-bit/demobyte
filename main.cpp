// Dem0!Byt3 V3 — bytebeat + floatbeat hybrid + drums
//
// Motor de síntesis: mezcla morph continuo bytebeat ↔ floatbeat
// Floatbeat: osciladores float con sine/tri/softclip (4 algos × 3 familias)
// Bytebeat:  17 fórmulas enteras clásicas
// Morph:     pot izquierda=bytebeat puro, derecha=floatbeat puro
//
// PADS:
//   GP8  → nueva fórmula aleatoria + LED
//   GP9  → kick
//   GP13 → snare
//   GP14 → hat
//
// ── PARÁMETROS DE PAD — ajustar acá ──────────────────────────────
// Si los pads no responden:  bajar PAD_THRESHOLD_ON  (ej: 0.18f)
// Si se disparan solos:      subir PAD_THRESHOLD_ON  (ej: 0.45f)
// Si hay crosstalk:          subir PAD_CONFIRM_NEEDED (ej: 4)
#define PAD_DISCHARGE_US    20000   // 20ms — descarga del cap entre medidas
#define PAD_MAX_US          60000   // 60ms — timeout por pad
#define PAD_THRESHOLD_ON    0.22f   // 22% del baseline para activar
#define PAD_THRESHOLD_OFF   0.12f   // 12% del baseline para soltar
#define PAD_CONFIRM_NEEDED  2       // scans consecutivos para confirmar toque
#define PAD_BASELINE_ALPHA  0.003f  // velocidad de adaptación del baseline

#include <cstdint>
#include <cmath>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/adc.h"
#include "hardware/timer.h"
#include "pcm5102_i2s.pio.h"
#include "audio/drums/drum_engine.h"

// ── I2S ───────────────────────────────────────────────────────────
static PIO  g_pio = pio0;
static uint g_sm  = 0;
static inline void i2s_write(int16_t l, int16_t r) {
    pio_sm_put_blocking(g_pio, g_sm, (uint32_t)(uint16_t)l << 16);
    pio_sm_put_blocking(g_pio, g_sm, (uint32_t)(uint16_t)r << 16);
}

// ── Pines ─────────────────────────────────────────────────────────
constexpr uint PIN_ROW     = 5;
constexpr uint PIN_COL[4]  = {8, 9, 13, 14};
constexpr uint PIN_LED     = 25;
constexpr uint PIN_POT     = 26;
constexpr uint PIN_BCLK    = 10;
constexpr uint PIN_DIN     = 12;
constexpr uint SAMPLE_RATE = 44100;

// ── Shared state ──────────────────────────────────────────────────
static volatile float   g_pot       = 0.5f;
static volatile uint8_t g_pad_event = 0;
static volatile bool    g_ready     = false;

// ── RNG ───────────────────────────────────────────────────────────
static uint32_t g_rng = 0xDEADBEEFu;
static inline uint32_t rng_next() {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return g_rng;
}
static inline float rng_f() {
    return float(rng_next() >> 8) * (1.0f / 16777215.0f);
}

// ── Float helpers ─────────────────────────────────────────────────
static inline float f_wrap(float x) {
    x -= (int)x; return x < 0.0f ? x + 1.0f : x;
}
static inline float f_sine(float p) {
    // fast sine from phase 0..1
    float x = f_wrap(p) * 6.28318f - 3.14159f;
    float y = x * (1.2732f - 0.4053f * fabsf(x));
    return y * (0.225f * (fabsf(y) - 1.0f) + 1.0f) * 0.95f;
}
static inline float f_tri(float p) {
    float x = f_wrap(p);
    return 4.0f * fabsf(x - 0.5f) - 1.0f;
}
static inline float f_clip(float x) {
    return x / (1.0f + fabsf(x));
}

// ── Bytebeat — 17 fórmulas ────────────────────────────────────────
static inline uint8_t bb_formula(uint8_t id, uint32_t t, uint8_t s) {
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

// ── Floatbeat — 8 algos basados en floatbeat_seed.h del firmware ──
// base_hz: frecuencia base (pot controla esto: 55..880 Hz)
// body:    0=seco/simple, 1=lleno/complejo
// t_f:     fase acumulada (avanza por dt cada sample)
struct FbState { float t = 0.0f; };

static inline float fb_algo(FbState& st, float dt, float hz, float body, uint8_t algo) {
    st.t += dt;
    if (st.t > 4096.0f) st.t -= 4096.0f;
    const float t = st.t;
    hz = hz < 20.0f ? 20.0f : (hz > 4000.0f ? 4000.0f : hz);
    body = body < 0.0f ? 0.0f : (body > 1.0f ? 1.0f : body);

    switch (algo % 8u) {
    case 0: {   // deep — sine stack con sub
        float x = f_sine(t*hz)*(0.54f+0.18f*body)
                + f_sine(t*(hz*0.5f))*(0.18f+0.30f*body)
                + f_sine(t*(hz*1.5f))*0.08f
                + f_sine(t*(hz*0.25f))*0.10f;
        return f_clip(x);
    }
    case 1: {   // metallic — sine + tri harmónicos altos
        float x = f_sine(t*hz)*(0.40f+0.12f*body)
                + f_tri(t*(hz*2.01f))*0.30f
                + f_sine(t*(hz*3.97f))*0.11f;
        return f_clip(x*0.90f);
    }
    case 2: {   // sub bass — énfasis en subgrave
        float x = f_sine(t*(hz*0.5f))*(0.56f+0.34f*body)
                + f_sine(t*hz)*(0.28f+0.16f*body)
                + f_tri(t*(hz*0.25f))*0.10f;
        return f_clip(x);
    }
    case 3: {   // noisy — wobble + textura
        float wob = f_sine(t*0.73f)*0.11f;
        float x = f_tri(t*(hz*(1.0f+wob)))*0.32f
                + f_sine(t*(hz*1.33f))*(0.34f+0.10f*body)
                + f_sine(t*(hz*0.5f))*0.18f;
        return f_clip(x*0.92f);
    }
    case 4: {   // parallel lanes — dos capas desfasadas
        float ta = t*hz, tb = (t+0.5f)*(hz*0.751f);
        float x = f_sine(ta)*0.42f
                + f_tri(tb)*0.26f
                + f_sine(ta*0.5f+tb*0.125f)*0.18f;
        return f_clip(x);
    }
    case 5: {   // FM — modulación de frecuencia simple
        float mod = f_sine(t*(hz*0.5f))*(0.20f+0.40f*body);
        float x = f_sine(t*hz + mod)*(0.65f+0.20f*body)
                + f_sine(t*(hz*2.0f)+mod*0.5f)*0.18f;
        return f_clip(x);
    }
    case 6: {   // ring — multiplicación de dos osciladores
        float a = f_sine(t*hz);
        float b = f_sine(t*(hz*1.41f+0.07f));
        float sub = f_sine(t*(hz*0.5f))*(0.20f+0.25f*body);
        return f_clip(a*b*0.75f + sub);
    }
    case 7: {   // tri fold — tri con wavefold
        float x = f_tri(t*hz)*(1.2f+body*1.5f);
        // wavefold: refleja cuando supera ±1
        while (x > 1.0f)  x = 2.0f - x;
        while (x < -1.0f) x = -2.0f - x;
        return f_clip(x * (0.60f+0.25f*body));
    }
    default: return 0.0f;
    }
}

// ── Motor híbrido bytebeat ↔ floatbeat ───────────────────────────
struct HybridSynth {
    // bytebeat params
    uint8_t  bb_fa = 2, bb_fb = 10, bb_morph = 128, bb_seed = 0;
    uint16_t bb_rate = 1;

    // floatbeat params
    FbState  fb_st_a, fb_st_b;
    uint8_t  fb_algo_a = 0, fb_algo_b = 3;
    float    fb_hz     = 110.0f;  // frecuencia base
    float    fb_body   = 0.5f;
    float    fb_morph  = 0.5f;   // 0=solo algo_a, 1=solo algo_b

    // morph global bytebeat ↔ floatbeat (controlado por pot)
    float    domain_morph = 0.5f;  // 0=bytebeat puro, 1=floatbeat puro

    // crossfade al randomizar
    float    xfade_pos  = 1.0f;   // 0..1, 1=nuevo completo
    float    xfade_prev = 0.0f;   // sample previo para crossfade

    // DC blockers
    int32_t  bb_dcx = 0, bb_dcy = 0;
    float    fb_dcx = 0, fb_dcy = 0;
    float    lp_z   = 0.0f;       // LP suave global

    void randomize(float pot) {
        // bytebeat
        bb_fa   = (uint8_t)(rng_next() % 17u);
        bb_fb   = (uint8_t)(rng_next() % 17u);
        bb_morph= (uint8_t)(rng_next() >> 24);
        bb_rate = (uint16_t)(1u + rng_next() % 3u);
        bb_seed = (uint8_t)(rng_next() >> 24);

        // floatbeat
        fb_algo_a = (uint8_t)(rng_next() % 8u);
        fb_algo_b = (uint8_t)(rng_next() % 8u);
        fb_hz     = 55.0f * powf(2.0f, pot * 4.0f);  // 55..880 Hz según pot
        fb_body   = 0.2f + rng_f() * 0.6f;
        fb_morph  = rng_f();

        xfade_pos = 0.0f;  // inicia crossfade
    }

    int16_t next(uint32_t t, float pot) {
        // dt para floatbeat: 1/44100 segundos por sample
        constexpr float DT = 1.0f / 44100.0f;

        // ── Bytebeat ──────────────────────────────────────────────
        const uint32_t ts = t / (bb_rate ? bb_rate : 1u);
        uint8_t va = bb_formula(bb_fa, ts, bb_seed);
        uint8_t vb = bb_formula(bb_fb,
                                ts ^ (uint32_t)(bb_seed * 0x55u),
                                bb_seed ^ 0xA5u);
        uint8_t bb_raw = (uint8_t)(((uint16_t)va*(255u-bb_morph)
                                  + (uint16_t)vb*bb_morph) >> 8);
        // centrar y normalizar a float -1..1
        float bb_f = float((int8_t)(bb_raw ^ 0x80u)) * (1.0f / 128.0f);

        // DC blocker bytebeat
        int32_t bb_s32 = (int32_t)(bb_f * 32767.0f);
        int32_t bb_y = bb_s32 - bb_dcx + ((bb_dcy * 252) >> 8);
        bb_dcx = bb_s32; bb_dcy = bb_y;
        bb_f = float(bb_y) * (1.0f / 32767.0f);

        // ── Floatbeat ─────────────────────────────────────────────
        // Actualizar hz con pot (55..880 Hz)
        const float target_hz = 55.0f * powf(2.0f, pot * 4.0f);
        fb_hz += 0.001f * (target_hz - fb_hz);  // suavizado para evitar clicks

        float fa = fb_algo(fb_st_a, DT, fb_hz,        fb_body, fb_algo_a);
        float fb = fb_algo(fb_st_b, DT, fb_hz * 1.5f, fb_body, fb_algo_b);
        float fb_f = fa * (1.0f - fb_morph) + fb * fb_morph;

        // DC blocker floatbeat
        float fb_y = fb_f - fb_dcx + fb_dcy * (252.0f/256.0f);
        fb_dcx = fb_f; fb_dcy = fb_y;
        fb_f = fb_y;

        // ── Domain morph bytebeat ↔ floatbeat ─────────────────────
        // pot: 0=bytebeat, 0.5=50/50, 1=floatbeat
        domain_morph = pot;
        float out = bb_f * (1.0f - domain_morph) + fb_f * domain_morph;

        // ── LP suave global ────────────────────────────────────────
        // corte fijo, solo suaviza el BB para quitarle aliasing
        lp_z += 0.15f * (out - lp_z);
        // mezclar BB crudo con LP según domain_morph
        out = out * (0.3f + domain_morph * 0.5f)
            + lp_z * (0.7f - domain_morph * 0.5f);

        // ── Crossfade al randomizar ────────────────────────────────
        if (xfade_pos < 1.0f) {
            out = xfade_prev * (1.0f - xfade_pos) + out * xfade_pos;
            xfade_pos += 1.0f / 256.0f;  // crossfade en 256 samples ~5.8ms
        }
        xfade_prev = out;

        // Clamp y convertir a int16
        if (out >  0.95f) out =  0.95f;
        if (out < -0.95f) out = -0.95f;
        return (int16_t)(out * 32767.0f);
    }
};

// ── Pad sensing ───────────────────────────────────────────────────
static float   pad_base[4]    = {};
static bool    pad_on[4]      = {};
static bool    pad_prev[4]    = {};
static uint8_t pad_confirm[4] = {};

static uint32_t measure_pad(uint8_t c) {
    gpio_put(PIN_ROW, 0);
    sleep_us(PAD_DISCHARGE_US);
    const uint32_t t0 = time_us_32();
    gpio_put(PIN_ROW, 1);
    while (!gpio_get(PIN_COL[c])) {
        if ((time_us_32()-t0) >= (uint32_t)PAD_MAX_US) {
            gpio_put(PIN_ROW,0); return PAD_MAX_US;
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
        const float d      = float(raw) - pad_base[c];
        const float on_th  = pad_base[c] * PAD_THRESHOLD_ON;
        const float off_th = pad_base[c] * PAD_THRESHOLD_OFF;

        if (!pad_on[c]) {
            if (d >= on_th) {
                if (++pad_confirm[c] >= PAD_CONFIRM_NEEDED)
                    pad_on[c] = true;
            } else {
                pad_confirm[c] = 0;
                pad_base[c] += PAD_BASELINE_ALPHA * (float(raw) - pad_base[c]);
            }
        } else {
            if (d < off_th) {
                pad_on[c] = false;
                pad_confirm[c] = 0;
                pad_base[c] += PAD_BASELINE_ALPHA * (float(raw) - pad_base[c]);
            }
        }
    }
}

// ── Core1: pads + pot ─────────────────────────────────────────────
static void core1_main() {
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
    while (true) {
        pot_s += 0.08f * (float(adc_read()) / 4095.0f - pot_s);
        g_pot = pot_s;
        scan_pads();
        if (pad_on[0] && !pad_prev[0]) { g_pad_event |= 1u; gpio_put(PIN_LED,1); }
        if (!pad_on[0])                   gpio_put(PIN_LED, 0);
        if (pad_on[1] && !pad_prev[1])  g_pad_event |= 2u;
        if (pad_on[2] && !pad_prev[2])  g_pad_event |= 4u;
        if (pad_on[3] && !pad_prev[3])  g_pad_event |= 8u;
    }
}

// ── Core0: audio ──────────────────────────────────────────────────
int main() {
    // Sin set_sys_clock_khz — igual que prueba_i2s_andando
    const uint off = pio_add_program(g_pio, &pcm5102_i2s_program);
    g_sm = pio_claim_unused_sm(g_pio, true);
    pcm5102_i2s_program_init(g_pio, g_sm, off, PIN_DIN, PIN_BCLK, SAMPLE_RATE);
    for (int i = 0; i < 16; ++i) i2s_write(0, 0);

    static DrumEngine drums;
    drums.init();
    drums.set_params(0.3f, 0.5f, 1.0f);

    static HybridSynth synth;

    multicore_launch_core1(core1_main);
    while (!g_ready) sleep_ms(10);

    uint32_t t  = 0;
    uint32_t cr = 0;

    while (true) {
        if (++cr >= 32u) {
            cr = 0;
            const uint8_t ev = g_pad_event;
            if (ev) {
                g_pad_event = 0;
                if (ev & 1u) synth.randomize(g_pot);
                if (ev & 2u) drums.trigger(DRUM_KICK);
                if (ev & 4u) drums.trigger(DRUM_SNARE);
                if (ev & 8u) drums.trigger(DRUM_HAT);
            }
            // Pot → drum color/decay
            const float pot = g_pot;
            drums.set_params(pot * 0.8f, 0.2f + pot * 0.6f, -1.0f);
        }

        const int16_t synth_s = synth.next(t, g_pot);

        int16_t drum_l = 0, drum_r = 0;
        int32_t sc_q15 = 32767;
        drums.process(drum_l, drum_r, sc_q15);

        // Sidechain: kick duckea el synth
        const int32_t s_ducked = ((int32_t)synth_s * sc_q15) >> 15;

        int32_t out_l = s_ducked + (int32_t)drum_l;
        int32_t out_r = s_ducked + (int32_t)drum_r;
        if (out_l >  32767) out_l =  32767;
        if (out_l < -32768) out_l = -32768;
        if (out_r >  32767) out_r =  32767;
        if (out_r < -32768) out_r = -32768;

        i2s_write((int16_t)out_l, (int16_t)out_r);
        ++t;
    }
}
