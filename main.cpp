// main.cpp — BYT3 V3.6
//
// PAD 0 (GP8)  → nueva fórmula bytebeat (sin click: crossfade suave)
// PAD 1 (GP9)  → kick drum
// PAD 2 (GP13) → snare
// PAD 3 (GP14) → hat
// POT  (GP26)  → macro/tonal (timbre)
//
// Discharge 50ms para pads grandes de cobre grabado.
// Sin regeneración automática — solo al tocar pad 0.
// Bytebeat a volumen completo, drums con duck automático.

#include <cstdint>
#include <cstdio>
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
volatile float   g_pot        = 0.5f;
volatile uint8_t g_pad_event  = 0;    // bits: 0=new_bb, 1=kick, 2=snare, 3=hat
volatile bool    g_ready      = false;

// ── RNG ───────────────────────────────────────────────────────────
static uint32_t g_rng = 0xDEADBEEFu;
static inline uint32_t rng_next() {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return g_rng;
}

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
    gpio_put(PIN_ROW, 0);
    sleep_ms(100);
    for (uint8_t c = 0; c < 4; ++c) {
        uint64_t sum = 0;
        for (int s = 0; s < 20; ++s) sum += measure_pad(c);
        pad_base[c] = float(sum / 20);
        printf("pad%u baseline=%.0fus\n", c, (double)pad_base[c]);
    }
}

static void scan_pads() {
    for (uint8_t c = 0; c < 4; ++c) {
        pad_prev[c] = pad_on[c];
        const uint32_t raw = measure_pad(c);
        const float delta  = (float)raw - pad_base[c];
        const float hyst_on  = pad_base[c] * 0.15f;
        const float hyst_off = pad_base[c] * 0.08f;
        pad_on[c] = pad_on[c] ? (delta >= hyst_off) : (delta >= hyst_on);
        if (!pad_on[c])
            pad_base[c] += 0.01f * ((float)raw - pad_base[c]);
    }
}

static inline bool just_pressed(uint8_t n) { return pad_on[n] && !pad_prev[n]; }

// ── Core1 ─────────────────────────────────────────────────────────
void core1_entry() {
    gpio_init(PIN_LED); gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_init(PIN_ROW); gpio_set_dir(PIN_ROW, GPIO_OUT);
    gpio_put(PIN_ROW, 0);
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
        pot_s += 0.12f * (float(adc_read()) / 4095.0f - pot_s);
        g_pot = pot_s;

        scan_pads();

        // Solo reportar just_pressed — sin triggers fantasma por nivel
        if (just_pressed(0)) { g_pad_event |= 1u; gpio_put(PIN_LED, 1); }
        if (just_pressed(1))   g_pad_event |= 2u;
        if (just_pressed(2))   g_pad_event |= 4u;
        if (just_pressed(3))   g_pad_event |= 8u;

        // LED off cuando pad 0 se suelta
        if (!pad_on[0]) gpio_put(PIN_LED, 0);
    }
}

// ── Drums ────────────────────────────────────────────────────────
static uint32_t kick_ph  = 0,  kick_env  = 0;
static uint32_t s_rng    = 0xABCDu, snare_env = 0;
static uint32_t hat_env  = 0;
static int32_t  duck_env = 256;  // Q8, 256=sin duck

static void trig_kick()  {
    kick_env = 0xFFFFu; kick_ph = 0;
    duck_env = 48;  // -14dB duck al kick
}
static void trig_snare() {
    snare_env = 0xFFFFu; s_rng = rng_next();
    duck_env = 80;  // -10dB
}
static void trig_hat()   {
    hat_env = 0x6000u;
    if (duck_env > 180) duck_env = 180;
}

static inline int16_t proc_kick() {
    if (!kick_env) return 0;
    kick_ph += 0x9000000u - (kick_env << 9);
    int16_t s = (int16_t)((((kick_ph >> 31) ? 28000 : -28000)
                           * (int32_t)kick_env) >> 16);
    kick_env = kick_env > 150u ? kick_env - 150u : 0u;
    return s;
}
static inline int16_t proc_snare() {
    if (!snare_env) return 0;
    s_rng ^= s_rng << 7; s_rng ^= s_rng >> 9; s_rng ^= s_rng << 8;
    int16_t s = (int16_t)(((int32_t)(int16_t)s_rng
                           * (int32_t)snare_env) >> 16);
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

// ── BytebeatGraph con crossfade ───────────────────────────────────
// Dos grafos: activo y entrante. Crossfade de 2048 samples al cambiar.
static BytebeatGraph g_graph_a;
static BytebeatGraph g_graph_b;
static bool     g_using_a    = true;
static int32_t  g_xfade      = 256;  // Q8, 256=100% A, 0=100% B
static bool     g_xfading    = false;

static void start_new_bb(uint8_t zone, const EvalContext& ctx) {
    // Generar en el grafo inactivo
    BytebeatGraph& next = g_using_a ? g_graph_b : g_graph_a;
    for (int attempt = 0; attempt < 8; ++attempt) {
        next.generate(
            rng_next() ^ to_ms_since_boot(get_absolute_time()),
            zone, make_zone(zone));
        EvalContext tc = ctx;
        int ns = 0;
        for (int i = 0; i < 512; ++i) {
            tc.t = (uint32_t)(i * 64);
            const int16_t s = next.preview_evaluate(tc);
            if (s > 64 || s < -64) ++ns;
        }
        if (ns > 51) break;
    }
    // Iniciar crossfade hacia el nuevo grafo
    g_xfading = true;
    g_xfade   = 256;  // empieza en el grafo actual
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

    uint8_t zone = 1;

    EvalContext ctx{};
    ctx.t = 0; ctx.macro = 0.5f; ctx.tonal = 0.5f;
    ctx.zone = zone; ctx.time_div = 1.0f; ctx.spread = 0.0f;
    ctx.quant_amount = 0.0f; ctx.scale_id = 1; ctx.root = 0;
    ctx.note_mode_active = false; ctx.note_pitch_ratio = 1.0f;
    ctx.drum_color = 0.3f; ctx.drum_decay = 0.5f;
    ctx.breath_amount = 0.2f; ctx.harmonic_blend = 0.3f;

    // Generar fórmula inicial en graph_a
    for (int attempt = 0; attempt < 8; ++attempt) {
        g_graph_a.generate(rng_next() ^ 0xDEADBEEFu, zone, make_zone(zone));
        EvalContext tc = ctx; int ns = 0;
        for (int i = 0; i < 512; ++i) {
            tc.t = (uint32_t)(i * 64);
            const int16_t s = g_graph_a.preview_evaluate(tc);
            if (s > 64 || s < -64) ++ns;
        }
        if (ns > 51) break;
    }

    uint32_t cr = 0;

    while (true) {
        if (++cr >= 32u) {
            cr = 0;

            const float pot = g_pot;
            ctx.tonal = pot;
            ctx.macro = pot * 0.8f;

            const uint8_t ev = g_pad_event;
            if (ev) {
                g_pad_event = 0;
                if (ev & 1u) start_new_bb(zone, ctx);
                if (ev & 2u) trig_kick();
                if (ev & 4u) trig_snare();
                if (ev & 8u) trig_hat();
            }

            // Duck recovery
            duck_env += (256 - duck_env) >> 4;
            if (duck_env > 254) duck_env = 256;
        }

        ctx.t++;

        // Crossfade entre grafos
        int16_t synth;
        if (g_xfading) {
            const int16_t sa = g_graph_a.evaluate(ctx);
            const int16_t sb = g_graph_b.evaluate(ctx);

            // g_xfade: 256=100%A, 0=100%B
            // Fade de 256→0 en 2048 steps
            synth = (int16_t)(((int32_t)sa * g_xfade
                             + (int32_t)sb * (256 - g_xfade)) >> 8);

            g_xfade -= 1;
            if (g_xfade <= 0) {
                g_xfade   = 0;
                g_xfading = false;
                g_using_a = !g_using_a;  // el nuevo grafo pasa a ser el activo
            }
        } else {
            synth = g_using_a ? g_graph_a.evaluate(ctx)
                              : g_graph_b.evaluate(ctx);
        }

        // Duck del bytebeat cuando hay drums
        synth = (int16_t)((int32_t)synth * duck_env >> 8);

        int32_t out = (int32_t)synth
                    + (int32_t)proc_kick()
                    + (int32_t)proc_snare()
                    + (int32_t)proc_hat();
        if (out >  32767) out =  32767;
        if (out < -32768) out = -32768;

        i2s_write((int16_t)out, (int16_t)out);
    }
}
