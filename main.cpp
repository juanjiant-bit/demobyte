// main_minimal.cpp — BYT3 hardware test V2.0
//
// Sigue el patrón exacto del código que ya funciona (demobyte_i2s_pot_pads):
//   Core0 = tight loop de audio, sin timer, sin IRQ
//   Core1 = escaneo de pads + ADC, escribe variables volatile
//
// Controles:
//   Pot P0  → macro bytebeat (timbre / densidad)
//   Pad 0   → randomiza el sonido bytebeat
//   Pad 1   → kick drum (ruido corto)
//   Pad 2   → snare drum (ruido medio)
//   Pad 3   → hat (clic corto)

#include <cstdint>
#include <cmath>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "pcm5102_i2s.pio.h"

#include "io/adc_handler.h"
#include "io/cap_pad_handler.h"

namespace {

// ── Pines ─────────────────────────────────────────────────────────
constexpr uint PIN_BCLK     = 10;
constexpr uint PIN_DIN      = 12;
constexpr uint PIN_LED      = 25;
constexpr uint32_t SAMPLE_RATE = 44100;

// ── Pad layout (matriz 3x5) ───────────────────────────────────────
constexpr uint8_t PAD_DRONE = 0;   // REC    → randomiza bytebeat
constexpr uint8_t PAD_KICK  = 1;   // PLAY   → kick
constexpr uint8_t PAD_SNARE = 2;   // SHIFT  → snare
constexpr uint8_t PAD_HAT   = 3;   // SNAP1  → hat

// ── Estado compartido Core0↔Core1 ────────────────────────────────
volatile uint32_t g_bytebeat_t   = 0;       // tiempo del bytebeat (escrito por Core0)
volatile float    g_macro        = 0.5f;    // 0..1, del pot
volatile uint8_t  g_formula      = 0;       // fórmula activa 0..7
volatile bool     g_randomize    = false;   // pedido de randomizar
volatile uint8_t  g_drum_trig    = 0;       // bitmask: bit0=kick, bit1=snare, bit2=hat
volatile bool     g_controls_ready = false;

// ── RNG simple ───────────────────────────────────────────────────
static uint32_t rng_state = 0xDEADBEEFu;
static inline uint32_t rng_next() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

// ── I2S write ────────────────────────────────────────────────────
static PIO g_pio;
static uint g_sm;

static inline void i2s_write(int16_t l, int16_t r) {
    pio_sm_put_blocking(g_pio, g_sm, (uint32_t)(uint16_t)l << 16);
    pio_sm_put_blocking(g_pio, g_sm, (uint32_t)(uint16_t)r << 16);
}

// ── Bytebeat engine ──────────────────────────────────────────────
// 8 fórmulas clásicas que siempre producen sonido audible y variado
static inline uint8_t bytebeat_eval(uint32_t t, uint8_t formula, uint8_t rate, uint8_t mask) {
    const uint32_t tr = t >> (rate >> 5);  // rate 0..7 -> shift 0..7
    uint32_t v;
    switch (formula & 7u) {
        case 0: v = (tr >> 6) ^ (tr >> 8);                             break;
        case 1: v = tr * ((tr >> 5 | tr >> 8) & 63);                  break;
        case 2: v = (tr >> 4) | (tr >> 8);                             break;
        case 3: v = tr * (tr >> 11 & tr >> 8 & 63);                   break;
        case 4: v = tr >> 3 ^ tr >> 8 ^ tr >> 12;                     break;
        case 5: v = (tr | tr >> 9 | tr >> 11) * ((tr >> 14) & 3);     break;
        case 6: v = tr * (tr >> 8 | tr >> 6) >> 4;                    break;
        case 7: v = (tr >> 5 ^ tr >> 12) * (tr >> 4 | tr >> 8);       break;
        default: v = tr; break;
    }
    return (uint8_t)((v & mask) ^ (v >> 8));
}

// ── Drum engines ─────────────────────────────────────────────────
static uint32_t kick_phase = 0;
static uint32_t kick_env   = 0;     // Q16, decae
static uint32_t snare_rng  = 0x12345678u;
static uint32_t snare_env  = 0;
static uint32_t hat_env    = 0;

static void trigger_kick()  { kick_env  = 0xFFFFu; kick_phase = 0; }
static void trigger_snare() { snare_env = 0xFFFFu; snare_rng = rng_next(); }
static void trigger_hat()   { hat_env   = 0x6000u; }

static inline int16_t process_kick() {
    if (kick_env == 0) return 0;
    kick_phase += 0x8000000u - (kick_env << 10);  // freq decae con env
    const int16_t osc = (kick_phase & 0x80000000u) ? 28000 : -28000;
    const int16_t out = (int16_t)(((int32_t)osc * (int32_t)kick_env) >> 16);
    kick_env = (kick_env > 180u) ? (kick_env - 180u) : 0u;
    return out;
}

static inline int16_t process_snare() {
    if (snare_env == 0) return 0;
    snare_rng ^= snare_rng << 7; snare_rng ^= snare_rng >> 9; snare_rng ^= snare_rng << 8;
    const int16_t noise = (int16_t)(snare_rng & 0xFFFFu);
    const int16_t out   = (int16_t)(((int32_t)noise * (int32_t)snare_env) >> 16);
    snare_env = (snare_env > 120u) ? (snare_env - 120u) : 0u;
    return out;
}

static inline int16_t process_hat() {
    if (hat_env == 0) return 0;
    uint32_t hr = hat_env * 0x9E3779B9u;
    const int16_t noise = (int16_t)(hr & 0xFFFFu);
    const int16_t out   = (int16_t)(((int32_t)noise * (int32_t)hat_env) >> 16);
    hat_env = (hat_env > 800u) ? (hat_env - 800u) : 0u;
    return out;
}

// ── Core1: control ───────────────────────────────────────────────
void core1_entry() {
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 1);

    AdcHandler adc;
    CapPadHandler pads;

    adc.init();
    pads.init(CapPadHandler::Preset::DRY());
    sleep_ms(50);
    pads.calibrate();

    gpio_put(PIN_LED, 0);
    g_controls_ready = true;

    bool prev_drone = false;
    bool prev_kick  = false;
    bool prev_snare = false;
    bool prev_hat   = false;

    while (true) {
        adc.poll();
        pads.scan();

        // Macro desde pot 0
        g_macro = adc.get(0);

        // Pad edge detection
        const bool drone = pads.is_pressed(PAD_DRONE);
        const bool kick  = pads.is_pressed(PAD_KICK);
        const bool snare = pads.is_pressed(PAD_SNARE);
        const bool hat   = pads.is_pressed(PAD_HAT);

        if (drone && !prev_drone) {
            // Nueva fórmula aleatoria al tocar PAD_DRONE
            g_formula   = (uint8_t)(rng_next() % 8u);
            g_randomize = true;
            gpio_put(PIN_LED, 1);
        }
        if (!drone && prev_drone) gpio_put(PIN_LED, 0);

        if (kick  && !prev_kick)  g_drum_trig |= 1u;
        if (snare && !prev_snare) g_drum_trig |= 2u;
        if (hat   && !prev_hat)   g_drum_trig |= 4u;

        prev_drone = drone;
        prev_kick  = kick;
        prev_snare = snare;
        prev_hat   = hat;

        sleep_ms(1);
    }
}

} // namespace

// ── Core0: audio tight loop ──────────────────────────────────────
int main() {
    set_sys_clock_khz(125000, true);
    stdio_init_all();

    // I2S init
    g_pio = pio0;
    const uint offset = pio_add_program(g_pio, &pcm5102_i2s_program);
    g_sm = pio_claim_unused_sm(g_pio, true);
    pcm5102_i2s_program_init(g_pio, g_sm, offset, PIN_DIN, PIN_BCLK, SAMPLE_RATE);
    for (int i = 0; i < 32; ++i) i2s_write(0, 0);

    // Arrancar Core1 (pads + ADC)
    multicore_launch_core1(core1_entry);

    // Esperar calibración de pads
    while (!g_controls_ready) i2s_write(0, 0);

    // Parámetros del bytebeat
    uint8_t  formula = 0;
    uint8_t  rate    = 3;    // velocidad/pitch base
    uint8_t  mask    = 0xFF;
    uint32_t t       = 0;

    // Envelope del drone
    uint32_t env_q16  = 0xFFFFu;  // siempre encendido al arranque
    bool     env_on   = true;

    while (true) {
        // Leer controles compartidos
        const float macro = g_macro;

        // Randomizar si se pidió
        if (g_randomize) {
            g_randomize = false;
            formula = g_formula;
            rate    = (uint8_t)(rng_next() % 6u);
            mask    = (uint8_t)(0xC0u | (rng_next() % 64u));
            env_q16 = 0xFFFFu;
            env_on  = true;
        }

        // Triggers de drums
        const uint8_t drum = g_drum_trig;
        if (drum) {
            g_drum_trig = 0;
            if (drum & 1u) trigger_kick();
            if (drum & 2u) trigger_snare();
            if (drum & 4u) trigger_hat();
        }

        // Bytebeat
        const uint8_t raw = bytebeat_eval(t, formula, rate, mask);
        // Macro controla el mix entre el bytebeat crudo y una versión
        // suavizada para que el pot sea audible
        const uint8_t macro_scaled = (uint8_t)(macro * 255.0f);
        const uint8_t mixed = (uint8_t)(
            ((uint32_t)raw * macro_scaled + (uint32_t)(raw >> 2) * (255u - macro_scaled)) >> 8
        );
        int16_t synth = (int16_t)((int)(mixed) - 128) << 7;

        // Drums
        const int16_t kick_s  = process_kick();
        const int16_t snare_s = process_snare();
        const int16_t hat_s   = process_hat();

        // Mix
        int32_t out = (int32_t)synth + (int32_t)kick_s + (int32_t)snare_s + (int32_t)hat_s;
        if (out >  32767) out =  32767;
        if (out < -32768) out = -32768;

        i2s_write((int16_t)out, (int16_t)out);
        ++t;
    }
}
