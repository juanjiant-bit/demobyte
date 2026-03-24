// main.cpp — BYT3 hardware test V3.1
//
// Pinout exacto del hardware físico:
//   GP5  = ROW (drive común de los 4 pads) + 100nF a GND
//   GP8  = SNAP pad (pad 0 = randomize bytebeat)
//   GP9  = KICK pad (pad 1)
//   GP13 = SNARE pad (pad 2)
//   GP14 = HAT pad (pad 3)
//   GP26 = pot directo (ADC0)
//   GP10 = BCK, GP11 = LCK, GP12 = DIN (I2S PCM5102)
//   GP25 = LED onboard
//
// Circuito por pad: GP5 --1MΩ-- PAD_COBRE -- GP_col (sin pulls internos)
//
// Motor: BytebeatGraph real del stage11

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

// ── Pines ─────────────────────────────────────────────────────────
constexpr uint PIN_ROW   = 5;
constexpr uint PIN_COL[4] = {8, 9, 13, 14};
constexpr uint PIN_LED   = 25;
constexpr uint PIN_POT   = 26;
constexpr uint PIN_BCLK  = 10;
constexpr uint PIN_DIN   = 12;

// ── I2S ───────────────────────────────────────────────────────────
static PIO  g_pio;
static uint g_sm;
static inline void i2s_write(int16_t l, int16_t r) {
    pio_sm_put_blocking(g_pio, g_sm, (uint32_t)(uint16_t)l << 16);
    pio_sm_put_blocking(g_pio, g_sm, (uint32_t)(uint16_t)r << 16);
}

// ── Estado compartido ─────────────────────────────────────────────
volatile float   g_macro         = 0.5f;
volatile bool    g_randomize     = false;
volatile uint8_t g_zone          = 1;
volatile bool    g_zone_changed  = false;
volatile uint8_t g_morph         = 128;
volatile bool    g_morph_changed = false;
volatile uint8_t g_rate          = 128;
volatile bool    g_rate_changed  = false;
volatile uint8_t g_drum_trig     = 0;
volatile bool    g_ready         = false;

// ── RNG ───────────────────────────────────────────────────────────
static uint32_t g_rng = 0x12345678u;
static inline uint32_t rng_next() {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return g_rng;
}

// ── Pad handler: 1 ROW, 4 COLs ───────────────────────────────────
// Mide el tiempo de carga capacitiva por pad.
// GP5 (ROW) descarga el capacitor, luego lo carga hacia HIGH.
// GPx (COL) detecta cuándo el pin sube → ese tiempo = carga capacitiva.
// Con dedo encima: más capacitancia → más tiempo de carga.
//
// Preset para pistas PCB pequeñas (~5x5mm):
//   discharge_us  = 60    — descarga rápida
//   max_charge_us = 1200  — timeout si no hay señal
//   hyst_on_us    = 8     — sensibilidad alta
//   hyst_off_us   = 5     — histéresis de release

struct PadState {
    float    baseline[4]  = {200, 200, 200, 200}; // µs, se calibra solo
    uint32_t raw[4]       = {};
    bool     pressed[4]   = {};
    bool     prev[4]      = {};
};

static PadState g_pads;

// Mide el tiempo de carga del pad col_idx (0..3)
static uint32_t measure_pad(uint8_t col_idx) {
    const uint col_pin = PIN_COL[col_idx];

    // Descargar
    gpio_put(PIN_ROW, 0);
    sleep_us(60);

    // Cargar y medir
    const uint32_t t0 = time_us_32();
    gpio_put(PIN_ROW, 1);
    while (!gpio_get(col_pin)) {
        if ((time_us_32() - t0) >= 1200u) {
            gpio_put(PIN_ROW, 0);
            return 1200u;
        }
    }
    const uint32_t dt = time_us_32() - t0;
    gpio_put(PIN_ROW, 0);
    return dt;
}

// Calibración: 80 muestras por pad, ~3 segundos total
static void calibrate_pads() {
    gpio_put(PIN_ROW, 0);
    sleep_ms(30);
    for (uint8_t c = 0; c < 4; ++c) {
        uint64_t sum = 0;
        for (uint8_t s = 0; s < 80; ++s) {
            sum += measure_pad(c);
            sleep_us(80);
        }
        g_pads.baseline[c] = float(sum / 80);
    }
}

// Scan de los 4 pads
static void scan_pads() {
    for (uint8_t c = 0; c < 4; ++c) {
        g_pads.prev[c] = g_pads.pressed[c];
        const uint32_t raw = measure_pad(c);
        g_pads.raw[c] = raw;

        const float base  = g_pads.baseline[c];
        const float delta = float(raw) - base;
        const bool was_on = g_pads.pressed[c];

        // Hysteresis: on a 8µs, off a 5µs por encima del baseline
        const bool is_on = was_on ? (delta >= 5.0f)
                                  : (delta >= 8.0f && raw < 1200u);
        g_pads.pressed[c] = is_on;

        // Actualizar baseline solo cuando no hay toque
        if (!is_on) {
            g_pads.baseline[c] += 0.008f * (float(raw) - base);
        }
    }
}

static inline bool just_pressed(uint8_t i) {
    return g_pads.pressed[i] && !g_pads.prev[i];
}

// ── Drums ────────────────────────────────────────────────────────
static uint32_t kick_ph = 0, kick_env = 0;
static uint32_t s_rng = 0xABCDu, snare_env = 0;
static uint32_t hat_env = 0;

static void trig_kick()  { kick_env = 0xFFFFu; kick_ph = 0; }
static void trig_snare() { snare_env = 0xFFFFu; s_rng = rng_next(); }
static void trig_hat()   { hat_env = 0x6000u; }

static inline int16_t proc_kick() {
    if (!kick_env) return 0;
    kick_ph += 0x9000000u - (kick_env << 9);
    int16_t s = (int16_t)((((kick_ph >> 31) ? 26000 : -26000) * (int32_t)kick_env) >> 16);
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

// ── Core1: pads + pot ─────────────────────────────────────────────
void core1_entry() {
    // GPIO setup
    gpio_init(PIN_LED);  gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_init(PIN_ROW);  gpio_set_dir(PIN_ROW, GPIO_OUT);
    gpio_put(PIN_ROW, 0);

    for (uint8_t c = 0; c < 4; ++c) {
        gpio_init(PIN_COL[c]);
        gpio_set_dir(PIN_COL[c], GPIO_IN);
        gpio_disable_pulls(PIN_COL[c]);
    }

    // ADC para pot directo GP26
    adc_init();
    adc_gpio_init(PIN_POT);
    adc_select_input(0);

    // Calibración: LED ON durante ~3s
    gpio_put(PIN_LED, 1);
    sleep_ms(30);
    calibrate_pads();
    gpio_put(PIN_LED, 0);

    g_ready = true;

    float pot_s = float(adc_read()) / 4095.0f;
    uint8_t zone  = 1;
    uint8_t morph = 128;
    uint8_t rate  = 128;

    while (true) {
        // Pot → macro
        pot_s += 0.12f * (float(adc_read()) / 4095.0f - pot_s);
        g_macro = pot_s;

        scan_pads();

        // Pad 0 (GP8, SNAP): randomize bytebeat
        if (just_pressed(0)) {
            g_randomize = true;
            gpio_put(PIN_LED, 1);
        }
        if (!g_pads.pressed[0]) gpio_put(PIN_LED, 0);

        // Pad 1 (GP9, KICK): kick drum
        if (just_pressed(1)) g_drum_trig |= 1u;

        // Pad 2 (GP13, SNARE): snare
        if (just_pressed(2)) g_drum_trig |= 2u;

        // Pad 3 (GP14, HAT): hat + cambia zona
        if (just_pressed(3)) {
            g_drum_trig |= 4u;
            zone = (uint8_t)((zone + 1u) % 5u);
            g_zone = zone;
            g_zone_changed = true;
        }

        sleep_ms(1);
    }
}

} // namespace

// ── Core0: audio tight loop ──────────────────────────────────────
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

    // BytebeatGraph
    BytebeatGraph graph;
    uint8_t zone = 1;
    graph.generate(rng_next() ^ 0xDEADBEEFu, zone, make_zone(zone));

    EvalContext ctx{};
    ctx.t                = 0;
    ctx.macro            = 0.5f;
    ctx.tonal            = 0.5f;
    ctx.zone             = zone;
    ctx.time_div         = 1.0f;
    ctx.spread           = 0.0f;
    ctx.quant_amount     = 0.0f;
    ctx.scale_id         = 1;
    ctx.root             = 0;
    ctx.note_mode_active = false;
    ctx.note_pitch_ratio = 1.0f;
    ctx.drum_color       = 0.3f;
    ctx.drum_decay       = 0.5f;
    ctx.breath_amount    = 0.0f;
    ctx.harmonic_blend   = 0.5f;

    uint32_t cr = 0;

    while (true) {
        if (++cr >= 32u) {
            cr = 0;
            ctx.macro = g_macro;

            if (g_randomize) {
                g_randomize = false;
                graph.generate(
                    rng_next() ^ to_ms_since_boot(get_absolute_time()),
                    zone, make_zone(zone));
            }
            if (g_zone_changed) {
                g_zone_changed = false;
                zone = g_zone;
                ctx.zone = zone;
                graph.generate(rng_next(), zone, make_zone(zone));
            }
            if (g_morph_changed) {
                g_morph_changed = false;
                graph.set_morph(g_morph);
            }
            if (g_rate_changed) {
                g_rate_changed = false;
                graph.set_rate(g_rate);
            }

            const uint8_t dt = g_drum_trig;
            if (dt) {
                g_drum_trig = 0;
                if (dt & 1u) trig_kick();
                if (dt & 2u) trig_snare();
                if (dt & 4u) trig_hat();
            }
        }

        ctx.t++;
        int16_t synth = graph.evaluate(ctx);

        // Drums más audibles: mezclar con ganancia balanceada
        int32_t out = ((int32_t)synth >> 1)    // synth a -6dB
                    + (int32_t)proc_kick()
                    + (int32_t)proc_snare()
                    + (int32_t)proc_hat();
        if (out >  32767) out =  32767;
        if (out < -32768) out = -32768;

        i2s_write((int16_t)out, (int16_t)out);
    }
}
