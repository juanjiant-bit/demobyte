// main_minimal.cpp — BYT3 hardware validation V1.0
//
// Valida el motor de audio real (AudioEngine + BytebeatGraph + StateManager)
// sobre el hardware minimal: 4 pads grandes de cobre + 1 pot + PCM5102.
//
// PATRÓN: Core0 tight loop (process_one_sample directo, sin timer IRQ)
//         Core1 pads + pot → StateManager + event queue
//
// PADS (hardware confirmado):
//   GP5  = ROW drive común + 100nF a GND
//   GP8  = PAD0 / SNAP  → randomiza bytebeat
//   GP9  = PAD1 / KICK  → kick drum
//   GP13 = PAD2 / SNARE → snare
//   GP14 = PAD3 / HAT   → hat
//   1MΩ en serie en cada COL (lado sense)
//
// POT: GP26 directo (sin MUX) → macro/tonal
//
// CALIBRACIÓN: LED prende ~4s al arrancar. No tocar pads durante ese tiempo.

#include <cstdint>
#include <cmath>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/timer.h"

#include "audio/audio_engine.h"
#include "audio/audio_output_i2s.h"
#include "state/state_manager.h"
#include "utils/ring_buffer.h"
#include "sequencer/event_types.h"
#include "utils/debug_log.h"

namespace {

// ── Pines ─────────────────────────────────────────────────────────
constexpr uint PIN_ROW    = 5;
constexpr uint PIN_COL[4] = {8, 9, 13, 14};
constexpr uint PIN_LED    = 25;
constexpr uint PIN_POT    = 26;

// ── Globals ───────────────────────────────────────────────────────
static RingBuffer<SequencerEvent, 128> g_event_queue;
static StateManager   g_state;
static AudioOutputI2S g_audio_out;
static AudioEngine    g_audio_engine;

// ── Shared state Core0 ↔ Core1 ───────────────────────────────────
volatile bool    g_ready      = false;
volatile float   g_pot        = 0.5f;
volatile uint8_t g_pad_event  = 0;   // bits: 0=randomize, 1=kick, 2=snare, 3=hat

// ── RNG ───────────────────────────────────────────────────────────
static uint32_t g_rng = 0x13579BDFu;
static inline uint32_t rng_next() {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return g_rng;
}
static inline float frand() {
    return float((rng_next() >> 8) & 0xFFFFu) * (1.0f / 65535.0f);
}
static inline float clamp01(float x) { return x<0?0:x>1?1:x; }
static inline float shaped_rand(float c, float s) {
    const float r = (frand()+frand()+frand())*(1.0f/3.0f);
    return clamp01(c + (r-0.5f)*s);
}

// ── Pad sensing — directo, sin CapPadHandler ─────────────────────
// discharge 50ms garantiza descarga completa de pads grandes de cobre

constexpr uint32_t DISCHARGE_US = 5000;   // 5ms — ajustar si pads no responden
constexpr uint32_t MAX_US       = 30000;   // 30ms timeout

static float    pad_base[4] = {};
static bool     pad_on[4]   = {};
static bool     pad_prev[4] = {};

static uint32_t measure_pad(uint8_t c) {
    gpio_put(PIN_ROW, 0);
    sleep_us(DISCHARGE_US);
    const uint32_t t0 = time_us_32();
    gpio_put(PIN_ROW, 1);
    while (!gpio_get(PIN_COL[c])) {
        if ((time_us_32()-t0) >= MAX_US) { gpio_put(PIN_ROW,0); return MAX_US; }
    }
    const uint32_t dt = time_us_32()-t0;
    gpio_put(PIN_ROW, 0);
    return dt;
}

static void calibrate_pads() {
    gpio_put(PIN_ROW, 0); sleep_ms(100);
    for (uint8_t c = 0; c < 4; ++c) {
        uint64_t sum = 0;
        for (int s = 0; s < 5; ++s) sum += measure_pad(c);
        pad_base[c] = float(sum / 5);
        printf("pad%u baseline=%.0fus\n", c, (double)pad_base[c]);
    }
}

static void scan_pads() {
    for (uint8_t c = 0; c < 4; ++c) {
        pad_prev[c] = pad_on[c];
        const uint32_t raw = measure_pad(c);
        const float d = (float)raw - pad_base[c];
        const float on_th  = pad_base[c] * 0.15f;
        const float off_th = pad_base[c] * 0.08f;
        pad_on[c] = pad_on[c] ? (d>=off_th) : (d>=on_th);
        if (!pad_on[c]) pad_base[c] += 0.01f*((float)raw - pad_base[c]);
    }
}

static inline bool just_pressed(uint8_t n) { return pad_on[n] && !pad_prev[n]; }

// ── Randomización del motor bytebeat ─────────────────────────────
static void randomize_engine(float macro) {
    const float chaos   = 0.25f + macro * 0.70f;
    const float motion  = 0.18f + macro * 0.72f;
    const float density = 0.20f + macro * 0.55f;

    g_state.set_patch_param(PARAM_FORMULA_A,   shaped_rand(macro*0.65f,        0.95f));
    g_state.set_patch_param(PARAM_FORMULA_B,   shaped_rand(1.0f-macro*0.45f,  0.95f));
    g_state.set_patch_param(PARAM_RATE,        shaped_rand(0.15f+motion*0.60f, 0.65f));
    g_state.set_patch_param(PARAM_SHIFT,       shaped_rand(0.30f+motion*0.35f, 0.55f));
    g_state.set_patch_param(PARAM_MASK,        shaped_rand(0.40f+density*0.35f,0.70f));
    g_state.set_patch_param(PARAM_FEEDBACK,    shaped_rand(0.08f+chaos*0.45f,  0.55f));
    g_state.set_patch_param(PARAM_JITTER,      shaped_rand(0.04f+chaos*0.55f,  0.60f));
    g_state.set_patch_param(PARAM_PHASE,       shaped_rand(0.15f+motion*0.40f, 0.65f));
    g_state.set_patch_param(PARAM_XOR_FOLD,    shaped_rand(0.05f+chaos*0.45f,  0.60f));
    g_state.set_patch_param(PARAM_BB_SEED,     shaped_rand(0.22f+chaos*0.50f,  0.70f));
    g_state.set_patch_param(PARAM_FILTER_MACRO,shaped_rand(0.18f+macro*0.60f,  0.65f));
    g_state.set_patch_param(PARAM_RESONANCE,   shaped_rand(0.06f+macro*0.45f,  0.45f));
    // ENV_MACRO alto para sonido sostenido (evita el gate corto interno)
    g_state.set_patch_param(PARAM_ENV_MACRO,   0.80f + frand()*0.15f);
    g_state.set_zone_live((uint8_t)(rng_next() % 5u));
}

// ── Enviar evento al queue ────────────────────────────────────────
static void push_event(EventType type, uint8_t target, float value) {
    SequencerEvent ev{};
    ev.tick   = 0u;
    ev.type   = type;
    ev.target = target;
    ev.value  = value;
    g_event_queue.push(ev);
}

// ── Core1: pads + pot ─────────────────────────────────────────────
void core1_main() {
    gpio_init(PIN_LED); gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_init(PIN_ROW); gpio_set_dir(PIN_ROW, GPIO_OUT); gpio_put(PIN_ROW, 0);
    for (uint8_t c = 0; c < 4; ++c) {
        gpio_init(PIN_COL[c]); gpio_set_dir(PIN_COL[c], GPIO_IN);
        gpio_disable_pulls(PIN_COL[c]);
    }

    adc_init(); adc_gpio_init(PIN_POT); adc_select_input(0);

    // LED ON durante calibración
    gpio_put(PIN_LED, 1);
    calibrate_pads();
    gpio_put(PIN_LED, 0);

    g_ready = true;

    float pot_s = float(adc_read()) / 4095.0f;

    while (true) {
        // Pot → macro + tonal (cambio muy audible en el filtro)
        pot_s += 0.12f * (float(adc_read())/4095.0f - pot_s);
        g_pot = pot_s;

        g_state.set_patch_param(PARAM_MACRO,        pot_s);
        g_state.set_patch_param(PARAM_TONAL,        pot_s);
        g_state.set_patch_param(PARAM_FILTER_MACRO, clamp01(0.20f + pot_s*0.70f));
        g_state.set_patch_param(PARAM_MORPH,        pot_s);
        g_state.set_bus_param(PARAM_REVERB_WET,     clamp01(0.05f + pot_s*0.20f));

        scan_pads();

        if (just_pressed(0)) {
            randomize_engine(pot_s);
            push_event(EVT_PAD_TRIGGER, 0, 1.0f);
            gpio_put(PIN_LED, 1);
        }
        if (!pad_on[0]) gpio_put(PIN_LED, 0);
        if (just_pressed(1)) push_event(EVT_DRUM_HIT, DRUM_KICK,  1.0f);
        if (just_pressed(2)) push_event(EVT_DRUM_HIT, DRUM_SNARE, 1.0f);
        if (just_pressed(3)) push_event(EVT_DRUM_HIT, DRUM_HAT,   1.0f);
    }
}

} // namespace

// ── Core0: audio tight loop ──────────────────────────────────────
int main() {
    set_sys_clock_khz(125000, true);
    stdio_init_all();
    sleep_ms(200);

    LOG("=== BYT3 minimal hardware validation ===");

    g_state.init();

    // Estado inicial que produce sonido inmediatamente
    g_state.set_patch_param(PARAM_FORMULA_A,   0.12f);
    g_state.set_patch_param(PARAM_FORMULA_B,   0.60f);
    g_state.set_patch_param(PARAM_RATE,        0.50f);
    g_state.set_patch_param(PARAM_SHIFT,       0.35f);
    g_state.set_patch_param(PARAM_MASK,        0.65f);
    g_state.set_patch_param(PARAM_FEEDBACK,    0.20f);
    g_state.set_patch_param(PARAM_FILTER_MACRO,0.50f);
    g_state.set_patch_param(PARAM_MACRO,       0.50f);
    g_state.set_patch_param(PARAM_TONAL,       0.50f);
    g_state.set_patch_param(PARAM_MORPH,       0.50f);
    // ENV_MACRO alto = sonido sostenido sin gate corto interno
    g_state.set_patch_param(PARAM_ENV_MACRO,   0.90f);
    g_state.set_patch_param(PARAM_ENV_ATTACK,  0.02f);
    g_state.set_patch_param(PARAM_ENV_RELEASE, 0.80f);
    g_state.set_bus_param(PARAM_REVERB_ROOM,   0.55f);
    g_state.set_bus_param(PARAM_REVERB_WET,    0.12f);
    g_state.set_bus_param(PARAM_DRUM_DECAY,    0.50f);
    g_state.set_bus_param(PARAM_DRUM_COLOR,    0.50f);
    g_state.set_bus_param(PARAM_DUCK_AMOUNT,   0.80f);

    // env_loop para drone continuo
    g_state.set_env_loop(true);

    g_audio_engine.init(&g_audio_out, &g_state);
    g_audio_engine.set_event_queue(&g_event_queue);

    // Arrancar Core1 (pads + pot)
    multicore_lockout_victim_init();
    multicore_launch_core1(core1_main);

    // Esperar calibración de pads antes de iniciar audio
    while (!g_ready) sleep_ms(10);

    LOG("Core0: entrando en audio loop");

    // TIGHT LOOP — process_one_sample() llama output_->write() internamente
    // que llama pio_sm_put_blocking → bloquea hasta que el FIFO tiene espacio
    // Esto sincroniza el loop exactamente con el sample rate del DAC
    while (true) {
        g_audio_engine.process_one_sample();
    }
}
