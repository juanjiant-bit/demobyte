// main.cpp — BYT3 hardware test V3.0
//
// Motor bytebeat real (BytebeatGraph del stage11) + pads auto-calibrados
// Patrón: Core0 tight loop I2S, Core1 controles volatile
//
// CONTROLES:
//   Pot P0   → macro (timbre principal, audible inmediatamente)
//   Pad  0   → nueva semilla + fórmula aleatoria, LED prende
//   Pad  1   → kick drum
//   Pad  2   → snare
//   Pad  3   → hat
//   Pad  4   → cambia zona (0..4: melódico→caos), cada toque +1
//   Pad  5   → morph +32 (mezcla entre fórmula A y B)
//   Pad  6   → rate +24 (pitch/velocidad)

#include <cstdint>
#include <cmath>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "pcm5102_i2s.pio.h"

#include "io/cap_pad_handler.h"
#include "synth/bytebeat_graph.h"
#include "synth/zone_config.h"

namespace {

constexpr uint PIN_BCLK = 10;
constexpr uint PIN_DIN  = 12;
constexpr uint PIN_LED  = 25;
constexpr uint PIN_POT  = 26;

static PIO  g_pio;
static uint g_sm;

static inline void i2s_write(int16_t l, int16_t r) {
    pio_sm_put_blocking(g_pio, g_sm, (uint32_t)(uint16_t)l << 16);
    pio_sm_put_blocking(g_pio, g_sm, (uint32_t)(uint16_t)r << 16);
}

// ── Estado compartido ─────────────────────────────────────────────
volatile float   g_macro        = 0.5f;
volatile bool    g_randomize    = false;
volatile uint8_t g_zone         = 1;
volatile bool    g_zone_changed = false;
volatile uint8_t g_morph        = 128;
volatile bool    g_morph_changed = false;
volatile uint8_t g_rate         = 128;
volatile bool    g_rate_changed  = false;
volatile uint8_t g_drum_trig    = 0;
volatile bool    g_ready        = false;

// ── RNG ───────────────────────────────────────────────────────────
static uint32_t g_rng = 0x12345678u;
static inline uint32_t rng_next() {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return g_rng;
}

// ── Preset tuneado para pistas PCB pequeñas ──────────────────────
static constexpr CapPadHandler::Preset PRESET_PCB() {
    CapPadHandler::Preset p{};
    p.discharge_us   = 60;
    p.max_charge_us  = 1200;
    p.hyst_on_us     = 8;
    p.hyst_off_us    = 5;
    p.at_range_us    = 60;
    p.at_curve       = 1.2f;
    p.calib_samples  = 60;
    p.baseline_alpha = 0.008f;
    return p;
}

// ── Drums ────────────────────────────────────────────────────────
static uint32_t kick_ph  = 0, kick_env  = 0;
static uint32_t s_rng    = 0xABCDu, snare_env = 0;
static uint32_t hat_env  = 0;

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

// ── Core1 ─────────────────────────────────────────────────────────
void core1_entry() {
    gpio_init(PIN_LED); gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 1);  // LED ON durante calibración

    adc_init();
    adc_gpio_init(PIN_POT);
    adc_select_input(0);

    CapPadHandler pads;
    pads.init(PRESET_PCB());
    sleep_ms(50);
    pads.calibrate();

    gpio_put(PIN_LED, 0);
    g_ready = true;

    float pot_s = float(adc_read()) / 4095.0f;
    bool prev[7] = {};
    uint8_t zone  = 1;
    uint8_t morph = 128;
    uint8_t rate  = 128;

    while (true) {
        // Pot → macro
        pot_s += 0.12f * (float(adc_read()) / 4095.0f - pot_s);
        g_macro = pot_s;

        pads.scan();

        bool cur[7];
        for (int i = 0; i < 7; ++i) cur[i] = pads.is_pressed(i);

        // Pad 0: randomize
        if (cur[0] && !prev[0]) { g_randomize = true; gpio_put(PIN_LED, 1); }
        if (!cur[0] && prev[0]) gpio_put(PIN_LED, 0);

        // Drums
        if (cur[1] && !prev[1]) g_drum_trig |= 1u;
        if (cur[2] && !prev[2]) g_drum_trig |= 2u;
        if (cur[3] && !prev[3]) g_drum_trig |= 4u;

        // Zona
        if (cur[4] && !prev[4]) {
            zone = (uint8_t)((zone + 1u) % 5u);
            g_zone = zone; g_zone_changed = true;
        }
        // Morph
        if (cur[5] && !prev[5]) {
            morph += 32u;
            g_morph = morph; g_morph_changed = true;
        }
        // Rate
        if (cur[6] && !prev[6]) {
            rate += 24u;
            g_rate = rate; g_rate_changed = true;
        }

        for (int i = 0; i < 7; ++i) prev[i] = cur[i];
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
    g_sm = pio_claim_unused_sm(g_pio, true);
    pcm5102_i2s_program_init(g_pio, g_sm, off, PIN_DIN, PIN_BCLK, 44100);
    for (int i = 0; i < 32; ++i) i2s_write(0, 0);

    multicore_launch_core1(core1_entry);
    while (!g_ready) i2s_write(0, 0);

    // ── BytebeatGraph ─────────────────────────────────────────────
    BytebeatGraph graph;
    uint8_t zone = 1;
    graph.generate(rng_next() ^ 0xDEADBEEFu, zone, make_zone(zone));

    // EvalContext — se actualiza cada 32 samples (control rate)
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

    uint32_t cr_ctr = 0;

    while (true) {
        // Control rate: cada 32 samples
        if (++cr_ctr >= 32u) {
            cr_ctr = 0;

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

        // Audio sample
        ctx.t++;
        int16_t synth = graph.evaluate(ctx);

        int32_t out = (int32_t)synth
                    + (int32_t)proc_kick()
                    + (int32_t)proc_snare()
                    + (int32_t)proc_hat();
        if (out >  32767) out =  32767;
        if (out < -32768) out = -32768;

        i2s_write((int16_t)out, (int16_t)out);
    }
}
