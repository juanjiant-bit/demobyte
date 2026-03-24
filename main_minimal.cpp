// main_minimal.cpp — BYT3 hardware test V1.2
//
// FIXES vs versión original de GPT:
//   FIX-1: ACCUM_TOP 10000->10 en audio_engine.h (44 samples/sec -> 44100/sec)
//   FIX-2: set_env_loop(true) — envelope continua, drone permanente
//   FIX-3: g_pads.init() después del primer trigger (no bloquear audio)
//   FIX-4: set_patch_param(PARAM_DELAY_DIV) en vez de set_bus_param

#include <cmath>
#include <cstdint>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "audio/audio_engine.h"
#include "audio/audio_output_i2s.h"
#include "io/minimal_touch_pads.h"
#include "state/state_manager.h"
#include "utils/ring_buffer.h"
#include "sequencer/event_types.h"
#include "utils/debug_log.h"

namespace {
constexpr uint PIN_POT = 26;
constexpr uint PIN_LED = 25;
constexpr uint8_t PAD_DRONE = 0;
constexpr uint8_t PAD_KICK  = 1;
constexpr uint8_t PAD_SNARE = 2;
constexpr uint8_t PAD_HAT   = 3;

static RingBuffer<SequencerEvent, 128> g_event_queue;
static StateManager   g_state;
static AudioOutputI2S g_audio_out;
static AudioEngine    g_audio_engine;
static MinimalTouchPads g_pads;
static float g_pot_smoothed = 0.5f;
static uint32_t g_rng = 0x13579BDFu;

static inline float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

static inline uint32_t rng_next() {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng;
}
static inline float frand() {
    return float((rng_next() >> 8) & 0xFFFFu) * (1.0f / 65535.0f);
}
static inline float shaped_rand(float center, float span) {
    const float r = (frand() + frand() + frand()) * (1.0f / 3.0f);
    return clamp01(center + (r - 0.5f) * span);
}

static float read_pot01() {
    const float target = float(adc_read()) * (1.0f / 4095.0f);
    g_pot_smoothed += 0.14f * (target - g_pot_smoothed);
    return g_pot_smoothed;
}

static void apply_macro_mapping(float macro) {
    g_state.set_patch_param(PARAM_MACRO,        macro);
    g_state.set_patch_param(PARAM_MORPH,        macro);
    g_state.set_patch_param(PARAM_DRIVE,        clamp01(0.10f + macro * 0.45f));
    g_state.set_patch_param(PARAM_FILTER_MACRO, clamp01(0.20f + macro * 0.70f));
    g_state.set_patch_param(PARAM_RESONANCE,    clamp01(0.10f + macro * 0.55f));
    g_state.set_bus_param(PARAM_DRUM_COLOR,     clamp01(0.12f + macro * 0.82f));
    g_state.set_bus_param(PARAM_DRUM_DECAY,     clamp01(0.28f + macro * 0.42f));
    g_state.set_bus_param(PARAM_DUCK_AMOUNT,    clamp01(0.60f + macro * 0.30f));
    g_state.set_bus_param(PARAM_CHORUS,         clamp01(0.04f + macro * 0.22f));
    g_state.set_bus_param(PARAM_REVERB_WET,     clamp01(0.06f + macro * 0.18f));
}

static void randomize_drone(float macro) {
    const float chaos   = 0.25f + macro * 0.70f;
    const float motion  = 0.18f + macro * 0.72f;
    const float density = 0.20f + macro * 0.55f;
    g_state.set_patch_param(PARAM_FORMULA_A,    shaped_rand(macro * 0.65f,          0.95f));
    g_state.set_patch_param(PARAM_FORMULA_B,    shaped_rand(1.0f - macro * 0.45f,   0.95f));
    g_state.set_patch_param(PARAM_RATE,         shaped_rand(0.15f + motion  * 0.60f, 0.65f));
    g_state.set_patch_param(PARAM_SHIFT,        shaped_rand(0.30f + motion  * 0.35f, 0.55f));
    g_state.set_patch_param(PARAM_MASK,         shaped_rand(0.40f + density * 0.35f, 0.70f));
    g_state.set_patch_param(PARAM_FEEDBACK,     shaped_rand(0.08f + chaos   * 0.45f, 0.55f));
    g_state.set_patch_param(PARAM_JITTER,       shaped_rand(0.04f + chaos   * 0.55f, 0.60f));
    g_state.set_patch_param(PARAM_PHASE,        shaped_rand(0.15f + motion  * 0.40f, 0.65f));
    g_state.set_patch_param(PARAM_XOR_FOLD,     shaped_rand(0.05f + chaos   * 0.45f, 0.60f));
    g_state.set_patch_param(PARAM_BB_SEED,      shaped_rand(0.22f + chaos   * 0.50f, 0.70f));
    g_state.set_patch_param(PARAM_FILTER_MACRO, shaped_rand(0.18f + macro   * 0.60f, 0.65f));
    g_state.set_patch_param(PARAM_RESONANCE,    shaped_rand(0.06f + macro   * 0.45f, 0.45f));
    g_state.set_patch_param(PARAM_ENV_MACRO,    shaped_rand(0.24f + motion  * 0.40f, 0.65f));
    g_state.set_zone_live((uint8_t)(rng_next() % 5u));
}

static void trigger_drone() {
    SequencerEvent ev{};
    ev.type = EVT_PAD_TRIGGER; ev.value = 1.0f;
    g_event_queue.push(ev);
}

static void send_drum(DrumId drum) {
    SequencerEvent ev{};
    ev.type = EVT_DRUM_HIT;
    ev.target = static_cast<uint8_t>(drum);
    ev.value = 1.0f;
    g_event_queue.push(ev);
}

static void core1_main() {
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 0);

    adc_init();
    adc_gpio_init(PIN_POT);
    adc_select_input(0);

    // Parámetros iniciales
    g_state.set_patch_param(PARAM_TONAL,       0.58f);
    g_state.set_patch_param(PARAM_SPREAD,      0.18f);
    g_state.set_patch_param(PARAM_ENV_ATTACK,  0.02f);
    g_state.set_patch_param(PARAM_ENV_RELEASE, 0.72f);
    g_state.set_patch_param(PARAM_TIME_DIV,    0.55f);
    g_state.set_patch_param(PARAM_GLIDE,       0.08f);
    g_state.set_patch_param(PARAM_DELAY_DIV,   0.40f);
    g_state.set_bus_param(PARAM_REVERB_ROOM,   0.62f);
    g_state.set_bus_param(PARAM_REVERB_WET,    0.14f);
    g_state.set_bus_param(PARAM_DELAY_FB,      0.22f);
    g_state.set_bus_param(PARAM_DELAY_WET,     0.10f);
    g_state.set_bus_param(PARAM_DUCK_AMOUNT,   0.82f);

    // FIX-2: envelope loop continua — sin esto el drone dura 2ms y desaparece
    g_state.set_env_loop(true);

    // Leer pot y arrancar drone
    g_pot_smoothed = float(adc_read()) * (1.0f / 4095.0f);
    apply_macro_mapping(g_pot_smoothed);
    randomize_drone(g_pot_smoothed);
    trigger_drone();
    gpio_put(PIN_LED, 1);

    // FIX-3: calibrar pads DESPUÉS del primer trigger para no bloquear el audio
    g_pads.init();
    gpio_put(PIN_LED, 0);

    while (true) {
        const float macro = read_pot01();
        apply_macro_mapping(macro);
        g_pads.scan();

        if (g_pads.just_pressed(PAD_DRONE)) {
            randomize_drone(macro);
            trigger_drone();
            gpio_put(PIN_LED, 1);
        }
        if (g_pads.just_released(PAD_DRONE)) gpio_put(PIN_LED, 0);
        if (g_pads.just_pressed(PAD_KICK))   send_drum(DRUM_KICK);
        if (g_pads.just_pressed(PAD_SNARE))  send_drum(DRUM_SNARE);
        if (g_pads.just_pressed(PAD_HAT))    send_drum(DRUM_HAT);

        sleep_us(1800);
    }
}
} // namespace

int main() {
    set_sys_clock_khz(125000, true);
    stdio_init_all();
    sleep_ms(250);
    LOG("=== BYT3 minimal hardware test V1.2 ===");

    g_state.init();
    g_audio_engine.init(&g_audio_out, &g_state);
    g_audio_engine.set_event_queue(&g_event_queue);

    multicore_lockout_victim_init();
    multicore_launch_core1(core1_main);

    g_audio_engine.run();
    return 0;
}
