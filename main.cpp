// main_minimal.cpp — BYT3 hardware test V2.1
//
// CAMBIOS vs V2.0:
//   FIX-POT: lee GP26 directamente con adc_read() sin pasar por MUX CD4051
//   FIX-PADS: preset tuneado para pistas PCB 5x5mm (baja capacitancia)
//   ADD: modo diagnóstico de pads por serial USB (ver DIAG_PADS abajo)
//
// DIAGNÓSTICO DE PADS:
//   Conectate por serial USB (115200 baud, cualquier terminal)
//   El firmware imprime cada 2 segundos los valores raw de cada pad:
//     baseline = sin dedo
//     raw      = con/sin dedo
//     delta    = raw - baseline (>0 = presión detectada)
//   Usá estos valores para tunear hyst_on en el preset.

#include <cstdint>
#include <cmath>
#include <cstdio>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "pcm5102_i2s.pio.h"

#include "io/cap_pad_handler.h"

namespace {

// ── Pines ─────────────────────────────────────────────────────────
constexpr uint PIN_BCLK     = 10;
constexpr uint PIN_DIN      = 12;
constexpr uint PIN_LED      = 25;
constexpr uint PIN_POT      = 26;   // GP26 directo, sin MUX
constexpr uint32_t SAMPLE_RATE = 44100;

// ── Pad layout ────────────────────────────────────────────────────
constexpr uint8_t PAD_DRONE = 0;
constexpr uint8_t PAD_KICK  = 1;
constexpr uint8_t PAD_SNARE = 2;
constexpr uint8_t PAD_HAT   = 3;

// ── Preset tuneado para pistas PCB 5x5mm ─────────────────────────
// Pistas chicas de cobre tienen capacitancia baja → tiempos de carga cortos.
// discharge_us bajo para no desperdiciar tiempo descargando.
// hyst_on bajo para mayor sensibilidad.
// Ajustá hyst_on según el diagnóstico serial:
//   sin dedo: delta ≈ 0..5us   → hyst_on = 8..12
//   con dedo: delta ≈ 15..80us → hyst_on = 8..12 (mitad del delta mínimo con dedo)
static constexpr CapPadHandler::Preset PRESET_PCB_SMALL() {
    CapPadHandler::Preset p{};
    p.discharge_us   = 80;    // bajo para PCB (era 150 en DRY)
    p.max_charge_us  = 1500;  // timeout antes de declarar pad sin conectar
    p.hyst_on_us     = 10;    // muy sensible para pistas chicas (era 30)
    p.hyst_off_us    = 6;     // histéresis de apagado
    p.at_range_us    = 80;    // rango para aftertouch
    p.at_curve       = 1.5f;
    p.calib_samples  = 80;    // menos muestras = calibración más rápida (~3s)
    p.baseline_alpha = 0.003f;
    return p;
}

// ── Estado compartido Core0↔Core1 ────────────────────────────────
volatile float    g_macro        = 0.5f;
volatile uint8_t  g_formula      = 0;
volatile bool     g_randomize    = false;
volatile uint8_t  g_drum_trig    = 0;
volatile bool     g_controls_ready = false;

// ── Diagnóstico de pads ──────────────────────────────────────────
// Cambiá a false para deshabilitar el output serial
static constexpr bool DIAG_PADS = true;
volatile uint32_t g_diag_raw[CapPadHandler::NUM_PADS]      = {};
volatile uint32_t g_diag_baseline[CapPadHandler::NUM_PADS] = {};

// ── RNG ───────────────────────────────────────────────────────────
static uint32_t rng_state = 0xDEADBEEFu;
static inline uint32_t rng_next() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

// ── I2S ───────────────────────────────────────────────────────────
static PIO  g_pio;
static uint g_sm;
static inline void i2s_write(int16_t l, int16_t r) {
    pio_sm_put_blocking(g_pio, g_sm, (uint32_t)(uint16_t)l << 16);
    pio_sm_put_blocking(g_pio, g_sm, (uint32_t)(uint16_t)r << 16);
}

// ── Pot directo GP26 ─────────────────────────────────────────────
static float g_pot_smooth = 0.5f;
static inline float read_pot() {
    const float raw = float(adc_read()) * (1.0f / 4095.0f);
    g_pot_smooth += 0.15f * (raw - g_pot_smooth);
    return g_pot_smooth;
}

// ── Bytebeat ─────────────────────────────────────────────────────
static inline uint8_t bytebeat_eval(uint32_t t, uint8_t formula, uint8_t rate) {
    const uint32_t tr = t >> (rate & 7u);
    switch (formula & 7u) {
        case 0: return (uint8_t)((tr >> 6) ^ (tr >> 8));
        case 1: return (uint8_t)(tr * ((tr >> 5 | tr >> 8) & 63u));
        case 2: return (uint8_t)((tr | tr >> 9 | tr >> 11) * ((tr >> 14) & 3u));
        case 3: return (uint8_t)(tr * (tr >> 11 & tr >> 8 & 63u));
        case 4: return (uint8_t)(tr >> 3 ^ tr >> 8 ^ tr >> 12);
        case 5: return (uint8_t)((tr >> 4) ^ (tr >> 7) ^ (tr >> 16));
        case 6: return (uint8_t)(tr * (tr >> 8 | tr >> 6) >> 4);
        case 7: return (uint8_t)((tr >> 5 ^ tr >> 12) * (tr >> 4 | tr >> 8));
        default: return (uint8_t)tr;
    }
}

// ── Drums ────────────────────────────────────────────────────────
static uint32_t kick_phase = 0, kick_env = 0;
static uint32_t snare_rng  = 0x12345678u, snare_env = 0;
static uint32_t hat_env    = 0;

static void trigger_kick()  { kick_env  = 0xFFFFu; kick_phase = 0; }
static void trigger_snare() { snare_env = 0xFFFFu; }
static void trigger_hat()   { hat_env   = 0x7000u; }

static inline int16_t proc_kick() {
    if (!kick_env) return 0;
    kick_phase += 0x9000000u - (kick_env << 9);
    int16_t out = (int16_t)((((kick_phase & 0x80000000u) ? 28000 : -28000) * (int32_t)kick_env) >> 16);
    kick_env = kick_env > 160u ? kick_env - 160u : 0u;
    return out;
}
static inline int16_t proc_snare() {
    if (!snare_env) return 0;
    snare_rng ^= snare_rng << 7; snare_rng ^= snare_rng >> 9; snare_rng ^= snare_rng << 8;
    int16_t out = (int16_t)(((int32_t)(int16_t)snare_rng * (int32_t)snare_env) >> 16);
    snare_env = snare_env > 110u ? snare_env - 110u : 0u;
    return out;
}
static inline int16_t proc_hat() {
    if (!hat_env) return 0;
    uint32_t h = hat_env * 0x9E3779B9u;
    int16_t out = (int16_t)(((int32_t)(int16_t)h * (int32_t)hat_env) >> 16);
    hat_env = hat_env > 700u ? hat_env - 700u : 0u;
    return out;
}

// ── Core1: pads + pot ─────────────────────────────────────────────
void core1_entry() {
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 1);

    // FIX-POT: inicializar ADC para GP26 directo, sin MUX
    adc_init();
    adc_gpio_init(PIN_POT);
    adc_select_input(0);  // ADC0 = GP26

    CapPadHandler pads;
    pads.init(PRESET_PCB_SMALL());
    sleep_ms(50);
    pads.calibrate();

    // Exponer baselines al diagnóstico
    if (DIAG_PADS) {
        for (uint8_t i = 0; i < CapPadHandler::NUM_PADS; ++i)
            g_diag_baseline[i] = pads.get_baseline_us(i);
    }

    gpio_put(PIN_LED, 0);
    g_controls_ready = true;

    bool prev[4] = {};
    uint32_t diag_timer = 0;

    while (true) {
        // Leer pot directo
        g_macro = read_pot();

        pads.scan();

        // Exponer raw para diagnóstico
        if (DIAG_PADS) {
            for (uint8_t i = 0; i < CapPadHandler::NUM_PADS; ++i)
                g_diag_raw[i] = pads.get_raw_us(i);
        }

        // Edge detection de los 4 pads activos
        const bool cur[4] = {
            pads.is_pressed(PAD_DRONE),
            pads.is_pressed(PAD_KICK),
            pads.is_pressed(PAD_SNARE),
            pads.is_pressed(PAD_HAT),
        };

        if (cur[0] && !prev[0]) {
            g_formula  = (uint8_t)(rng_next() % 8u);
            g_randomize = true;
            gpio_put(PIN_LED, 1);
        }
        if (!cur[0] && prev[0]) gpio_put(PIN_LED, 0);
        if (cur[1] && !prev[1]) g_drum_trig |= 1u;
        if (cur[2] && !prev[2]) g_drum_trig |= 2u;
        if (cur[3] && !prev[3]) g_drum_trig |= 4u;
        for (int i = 0; i < 4; ++i) prev[i] = cur[i];

        // Diagnóstico serial: imprime cada 2 segundos
        if (DIAG_PADS && ++diag_timer >= 2000u) {
            diag_timer = 0;
            printf("\n--- PAD DIAG (us) ---\n");
            printf("%-4s %-8s %-8s %-8s %-6s\n", "pad", "baseline", "raw", "delta", "state");
            for (uint8_t i = 0; i < CapPadHandler::NUM_PADS; ++i) {
                const uint32_t base  = g_diag_baseline[i];
                const uint32_t raw   = g_diag_raw[i];
                const int32_t  delta = (int32_t)raw - (int32_t)base;
                const bool     on    = pads.is_pressed(i);
                printf("%-4u %-8lu %-8lu %-8ld %-6s\n",
                       i, (unsigned long)base, (unsigned long)raw,
                       (long)delta, on ? "ON" : ".");
            }
            printf("pot=%.3f  macro=%.3f\n", (double)g_pot_smooth, (double)g_macro);
        }

        sleep_ms(1);
    }
}

} // namespace

// ── Core0: audio tight loop ──────────────────────────────────────
int main() {
    set_sys_clock_khz(125000, true);
    stdio_init_all();
    sleep_ms(100);

    g_pio = pio0;
    const uint offset = pio_add_program(g_pio, &pcm5102_i2s_program);
    g_sm = pio_claim_unused_sm(g_pio, true);
    pcm5102_i2s_program_init(g_pio, g_sm, offset, PIN_DIN, PIN_BCLK, SAMPLE_RATE);
    for (int i = 0; i < 32; ++i) i2s_write(0, 0);

    multicore_launch_core1(core1_entry);
    while (!g_controls_ready) i2s_write(0, 0);

    uint8_t  formula = 0, rate = 2;
    uint32_t t = 0;

    while (true) {
        const float macro = g_macro;

        if (g_randomize) {
            g_randomize = false;
            formula = g_formula;
            rate    = (uint8_t)(rng_next() % 5u);
        }

        const uint8_t drum = g_drum_trig;
        if (drum) {
            g_drum_trig = 0;
            if (drum & 1u) trigger_kick();
            if (drum & 2u) trigger_snare();
            if (drum & 4u) trigger_hat();
        }

        // Bytebeat: pot controla rate (pitch/velocidad) en rango audible
        // macro 0..1 → rate shift 0..4 → pitch de grave a agudo
        const uint8_t macro_rate = rate + (uint8_t)(macro * 3.0f);
        const uint8_t raw = bytebeat_eval(t, formula, macro_rate > 7u ? 7u : macro_rate);
        int16_t synth = (int16_t)((int)(raw) - 128) << 8;

        // Mix con drums
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
