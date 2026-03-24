// main.cpp — BYT3 hardware test V3.3
//
// PINOUT:
//   GP5=ROW (drive) + 100nF a GND
//   GP8=SNAP, GP9=KICK, GP13=SNARE, GP14=HAT (pads, COL)
//   Circuito: GP5 --1MΩ-- PAD_COBRE -- GP_col
//   GP26=POT, GP10=BCK, GP11=LCK, GP12=DIN, GP25=LED
//
// FIX DE PADS (análisis RC completo):
//   Con 1MΩ y ~15pF de capacitancia de pista PCB:
//     Sin dedo: ~10µs carga,  Con dedo (+50pF): ~43µs carga
//     Delta esperado: ~33µs
//     Discharge necesario: 95µs (5τ del cap de COL)
//   Preset anterior tenía max_charge_us=1200 (100x demasiado)
//   → todos los pads hacían timeout y baseline quedaba en 1200
//   → cualquier lectura real era MENOR que el baseline → delta negativo
//   → los pads aparecían siempre "presionados" (threshold invertido)

#include <cstdint>
#include <cmath>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/adc.h"
#include "hardware/timer.h"
#include "hardware/clocks.h"
#include "pcm5102_i2s.pio.h"
#include "synth/bytebeat_graph.h"
#include "synth/zone_config.h"

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
volatile float   g_pot          = 0.5f;
volatile bool    g_randomize    = false;
volatile uint8_t g_zone         = 1;
volatile bool    g_zone_changed = false;
volatile uint8_t g_drum_trig    = 0;
volatile int32_t g_duck         = 256;
volatile bool    g_ready        = false;

// ── RNG ───────────────────────────────────────────────────────────
static uint32_t g_rng = 0xDEADBEEFu;
static inline uint32_t rng_next() {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return g_rng;
}

// ── Pad sensing ───────────────────────────────────────────────────
//
// Valores calculados para: 1MΩ serie, ~15pF cap PCB, 100nF en ROW a GND
//   discharge_us = 95  → descarga completa del cap de COL (5τ = 75µs + margen)
//   max_charge_us = 200 → 4-5x el tiempo de carga con dedo (~43µs)
//   hyst_on  = 18      → ~mitad del delta esperado (33µs)
//   hyst_off = 10      → histéresis de release
//
// Si los pads siguen invertidos (noise sin tocar, silencio al tocar):
//   → aumentar discharge_us en pasos de 50µs
// Si no detectan el toque:
//   → bajar hyst_on en pasos de 4µs

constexpr uint32_t DISCHARGE_US  = 95;
constexpr uint32_t MAX_CHARGE_US = 200;
constexpr float    HYST_ON_US    = 18.0f;
constexpr float    HYST_OFF_US   = 10.0f;
constexpr float    BASELINE_ALPHA = 0.005f;

static float    pad_base[4] = {15, 15, 15, 15}; // inicio en ~15µs (bare pad)
static bool     pad_on[4]   = {};
static bool     pad_prev[4] = {};

static uint32_t measure_pad(uint8_t c) {
    // Paso 1: descargar el pad capacitor poniendo ROW en LOW
    gpio_put(PIN_ROW, 0);
    sleep_us(DISCHARGE_US);

    // Paso 2: cargar hacia HIGH y medir tiempo hasta que COL cruza el umbral
    const uint32_t t0 = time_us_32();
    gpio_put(PIN_ROW, 1);

    // Esperar brevemente a que el 100nF del ROW se cargue primero
    // (τ_row = 50Ω × 100nF = 5µs via GPIO, carga completa en ~25µs)
    // No hacemos sleep — el loop de polling es suficiente

    while (!gpio_get(PIN_COL[c])) {
        if ((time_us_32() - t0) >= MAX_CHARGE_US) {
            gpio_put(PIN_ROW, 0);
            return MAX_CHARGE_US;
        }
    }
    const uint32_t dt = time_us_32() - t0;
    gpio_put(PIN_ROW, 0);
    return dt;
}

static void calibrate_pads() {
    gpio_put(PIN_ROW, 0);
    sleep_ms(30);
    for (uint8_t c = 0; c < 4; ++c) {
        uint64_t sum = 0;
        // 100 muestras, ~20ms total por pad (95µs discharge + ~15µs charge × 100)
        for (uint8_t s = 0; s < 100; ++s) {
            sum += measure_pad(c);
            sleep_us(50);  // pequeña pausa entre medidas
        }
        pad_base[c] = float(sum / 100);
    }
}

static void scan_pads() {
    for (uint8_t c = 0; c < 4; ++c) {
        pad_prev[c] = pad_on[c];
        const uint32_t raw = measure_pad(c);
        const float delta = float(raw) - pad_base[c];
        const bool was = pad_on[c];
        // delta POSITIVO = más tiempo de carga = más capacitancia = dedo presente
        pad_on[c] = was ? (delta >= HYST_OFF_US) : (delta >= HYST_ON_US);
        // Actualizar baseline solo sin toque
        if (!pad_on[c]) {
            pad_base[c] += BASELINE_ALPHA * (float(raw) - pad_base[c]);
        }
    }
}

static inline bool just_pressed(uint8_t i)  { return  pad_on[i] && !pad_prev[i]; }
static inline bool just_released(uint8_t i) { return !pad_on[i] &&  pad_prev[i]; }

// ── Drums ────────────────────────────────────────────────────────
static uint32_t kick_ph   = 0, kick_env  = 0;
static uint32_t s_rng     = 0xABCDu, snare_env = 0;
static uint32_t hat_env   = 0;

static void trig_kick()  { kick_env  = 0xFFFFu; kick_ph  = 0; g_duck = 64; }
static void trig_snare() { snare_env = 0xFFFFu; s_rng = rng_next(); g_duck = 96; }
static void trig_hat()   { hat_env   = 0x6000u; if (g_duck > 160) g_duck = 160; }

static inline int16_t proc_kick() {
    if (!kick_env) return 0;
    kick_ph += 0x9000000u - (kick_env << 9);
    int16_t s = (int16_t)((((kick_ph >> 31) ? 28000 : -28000) * (int32_t)kick_env) >> 16);
    kick_env = kick_env > 150u ? kick_env - 150u : 0u;
    return s;
}
static inline int16_t proc_snare() {
    if (!snare_env) return 0;
    s_rng ^= s_rng << 7; s_rng ^= s_rng >> 9; s_rng ^= s_rng << 8;
    int16_t s = (int16_t)(((int32_t)(int16_t)s_rng * (int32_t)snare_env) >> 16);
    snare_env = snare_env > 100u ? snare_env - 100u : 0u;
    return s;
}
static inline int16_t proc_hat() {
    if (!hat_env) return 0;
    uint32_t h = hat_env * 0x9E3779B9u;
    int16_t s = (int16_t)(((int32_t)(int16_t)h * (int32_t)hat_env) >> 16);
    hat_env = hat_env > 600u ? hat_env - 600u : 0u;
    return s;
}

// ── generate() con retry para fórmulas silenciosas ────────────────
static void safe_generate(BytebeatGraph& graph, uint8_t zone,
                           const EvalContext& ref_ctx) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t seed = rng_next() ^ to_ms_since_boot(get_absolute_time());
        graph.generate(seed, zone, make_zone(zone));
        EvalContext ctx = ref_ctx;
        int nonsilent = 0;
        for (int i = 0; i < 512; ++i) {
            ctx.t = (uint32_t)(i * 64);
            if (graph.preview_evaluate(ctx) > 64 ||
                graph.preview_evaluate(ctx) < -64) ++nonsilent;
        }
        if (nonsilent > 51) return;
    }
    // Fallback: fórmula conocida
    graph.set_formula_a(2); graph.set_formula_b(5);
    graph.set_rate(128);    graph.set_mask(200);
}

// ── Core1 ─────────────────────────────────────────────────────────
void core1_entry() {
    gpio_init(PIN_LED); gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_init(PIN_ROW); gpio_set_dir(PIN_ROW, GPIO_OUT);
    gpio_put(PIN_ROW, 0);

    for (uint8_t c = 0; c < 4; ++c) {
        gpio_init(PIN_COL[c]);
        gpio_set_dir(PIN_COL[c], GPIO_IN);
        gpio_disable_pulls(PIN_COL[c]);
    }

    adc_init(); adc_gpio_init(PIN_POT); adc_select_input(0);

    // LED ON durante calibración
    gpio_put(PIN_LED, 1);
    calibrate_pads();
    gpio_put(PIN_LED, 0);
    g_ready = true;

    float pot_s  = float(adc_read()) / 4095.0f;
    uint8_t zone = 1;

    while (true) {
        pot_s += 0.12f * (float(adc_read()) / 4095.0f - pot_s);
        g_pot = pot_s;

        scan_pads();

        if (just_pressed(0))  { g_randomize = true; gpio_put(PIN_LED, 1); }
        if (just_released(0))   gpio_put(PIN_LED, 0);
        if (just_pressed(1))    g_drum_trig |= 1u;
        if (just_pressed(2))    g_drum_trig |= 2u;
        if (just_pressed(3)) {
            g_drum_trig |= 4u;
            zone = (uint8_t)((zone + 1u) % 5u);
            g_zone = zone; g_zone_changed = true;
        }

        sleep_ms(1);
    }
}

} // namespace

// ── Core0: audio ─────────────────────────────────────────────────
int main() {
    set_sys_clock_khz(125000, true);
    stdio_init_all();

    g_pio = pio0;
    const uint off = pio_add_program(g_pio, &pcm5102_i2s_program);
    g_sm  = pio_claim_unused_sm(g_pio, true);
    pcm5102_i2s_program_init(g_pio, g_sm, off, PIN_DIN, PIN_BCLK, 44100);
    for (int i = 0; i < 32; ++i) i2s_write(0, 0);

    multicore_launch_core1(core1_entry);
    while (!g_ready) i2s_write(0, 0);

    BytebeatGraph graph;
    uint8_t zone = 1;

    EvalContext ctx{};
    ctx.t = 0; ctx.macro = 0.5f; ctx.tonal = 0.5f;
    ctx.zone = zone; ctx.time_div = 1.0f; ctx.spread = 0.0f;
    ctx.quant_amount = 0.0f; ctx.scale_id = 1; ctx.root = 0;
    ctx.note_mode_active = false; ctx.note_pitch_ratio = 1.0f;
    ctx.drum_color = 0.3f; ctx.drum_decay = 0.5f;
    ctx.breath_amount = 0.2f; ctx.harmonic_blend = 0.3f;

    safe_generate(graph, zone, ctx);

    int32_t duck_smooth = 256;
    uint32_t cr = 0;

    while (true) {
        if (++cr >= 32u) {
            cr = 0;
            const float pot = g_pot;
            ctx.tonal = pot;
            ctx.macro = pot * 0.8f;

            if (g_randomize) {
                g_randomize = false;
                safe_generate(graph, zone, ctx);
            }
            if (g_zone_changed) {
                g_zone_changed = false;
                zone = g_zone; ctx.zone = zone;
                safe_generate(graph, zone, ctx);
            }

            const uint8_t dt = g_drum_trig;
            if (dt) {
                g_drum_trig = 0;
                if (dt & 1u) trig_kick();
                if (dt & 2u) trig_snare();
                if (dt & 4u) trig_hat();
            }

            // Duck recovery
            int32_t td = g_duck;
            duck_smooth += (td - duck_smooth) >> 3;
            if (duck_smooth >= 254) { duck_smooth = 256; g_duck = 256; }
        }

        ctx.t++;
        int16_t synth = graph.evaluate(ctx);
        synth = (int16_t)((int32_t)synth * duck_smooth >> 8);

        int32_t out = (int32_t)synth
                    + (int32_t)proc_kick()
                    + (int32_t)proc_snare()
                    + (int32_t)proc_hat();
        if (out >  32767) out =  32767;
        if (out < -32768) out = -32768;

        i2s_write((int16_t)out, (int16_t)out);
    }
}
