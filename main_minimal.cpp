// main_minimal.cpp — Dem0!Byt3 V1
//
// STRIPPED TEST: AudioEngine puro, sin Core1, sin pads, sin pot
// Si esto suena → el problema anterior era en Core1/interacción
// Si esto NO suena → el problema es en AudioEngine/StateManager
//
// Luego de confirmar audio, descomentar #define WITH_CONTROLS

// #define WITH_CONTROLS   // descomenta para activar pads + pot

#include <cstdint>
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

// ── Globals ───────────────────────────────────────────────────────
static RingBuffer<SequencerEvent, 128> g_event_queue;
static StateManager   g_state;
static AudioOutputI2S g_audio_out;
static AudioEngine    g_audio_engine;

#ifdef WITH_CONTROLS

// ── Pines pads/pot ────────────────────────────────────────────────
constexpr uint PIN_ROW    = 5;
constexpr uint PIN_COL[4] = {8, 9, 13, 14};
constexpr uint PIN_LED    = 25;
constexpr uint PIN_POT    = 26;

static volatile bool    g_ready     = false;
static volatile float   g_pot       = 0.5f;
static volatile uint8_t g_pad_event = 0;

static uint32_t g_rng = 0x13579BDFu;
static inline uint32_t rng_next() {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return g_rng;
}
static inline float frand() {
    return float((rng_next()>>8)&0xFFFFu)*(1.0f/65535.0f);
}
static inline float clamp01(float x) { return x<0?0:x>1?1:x; }
static inline float shaped_rand(float c,float s) {
    float r=(frand()+frand()+frand())*(1.0f/3.0f);
    return clamp01(c+(r-0.5f)*s);
}

static float    pad_base[4] = {};
static bool     pad_on[4]   = {};
static bool     pad_prev[4] = {};

static uint32_t measure_pad(uint8_t c) {
    gpio_put(PIN_ROW, 0); sleep_us(5000);
    const uint32_t t0 = time_us_32();
    gpio_put(PIN_ROW, 1);
    while (!gpio_get(PIN_COL[c]))
        if ((time_us_32()-t0) >= 30000u) { gpio_put(PIN_ROW,0); return 30000u; }
    const uint32_t dt = time_us_32()-t0;
    gpio_put(PIN_ROW, 0);
    return dt;
}

static void calibrate_pads() {
    gpio_put(PIN_ROW, 0); sleep_ms(50);
    for (uint8_t c = 0; c < 4; ++c) {
        uint64_t sum = 0;
        for (int s = 0; s < 5; ++s) sum += measure_pad(c);
        pad_base[c] = float(sum/5);
    }
}

static void scan_pads() {
    for (uint8_t c = 0; c < 4; ++c) {
        pad_prev[c] = pad_on[c];
        const uint32_t raw = measure_pad(c);
        const float d = float(raw)-pad_base[c];
        pad_on[c] = pad_on[c]?(d>=pad_base[c]*0.08f):(d>=pad_base[c]*0.15f);
        if (!pad_on[c]) pad_base[c] += 0.01f*(float(raw)-pad_base[c]);
    }
}

static void randomize_engine(float m) {
    float ch=0.25f+m*0.70f, mo=0.18f+m*0.72f, de=0.20f+m*0.55f;
    g_state.set_patch_param(PARAM_FORMULA_A,  shaped_rand(m*0.65f,    0.95f));
    g_state.set_patch_param(PARAM_FORMULA_B,  shaped_rand(1.0f-m*0.45f,0.95f));
    g_state.set_patch_param(PARAM_RATE,       shaped_rand(0.15f+mo*0.60f,0.65f));
    g_state.set_patch_param(PARAM_SHIFT,      shaped_rand(0.30f+mo*0.35f,0.55f));
    g_state.set_patch_param(PARAM_MASK,       shaped_rand(0.40f+de*0.35f,0.70f));
    g_state.set_patch_param(PARAM_FEEDBACK,   shaped_rand(0.08f+ch*0.45f,0.55f));
    g_state.set_patch_param(PARAM_JITTER,     shaped_rand(0.04f+ch*0.55f,0.60f));
    g_state.set_patch_param(PARAM_FILTER_MACRO,shaped_rand(0.18f+m*0.60f,0.65f));
    g_state.set_patch_param(PARAM_ENV_MACRO,  0.80f+frand()*0.15f);
    g_state.set_zone_live((uint8_t)(rng_next()%5u));
}

static void push_event(EventType type, uint8_t target, float value) {
    SequencerEvent ev{}; ev.type=type; ev.target=target; ev.value=value;
    g_event_queue.push(ev);
}

static void core1_main() {
    gpio_init(PIN_LED); gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_init(PIN_ROW); gpio_set_dir(PIN_ROW, GPIO_OUT); gpio_put(PIN_ROW,0);
    for (uint8_t c=0;c<4;++c) {
        gpio_init(PIN_COL[c]); gpio_set_dir(PIN_COL[c], GPIO_IN);
        gpio_disable_pulls(PIN_COL[c]);
    }
    adc_init(); adc_gpio_init(PIN_POT); adc_select_input(0);
    gpio_put(PIN_LED,1); calibrate_pads(); gpio_put(PIN_LED,0);
    g_ready = true;

    float pot_s = float(adc_read())/4095.0f;
    while (true) {
        pot_s += 0.12f*(float(adc_read())/4095.0f - pot_s);
        g_pot = pot_s;
        g_state.set_patch_param(PARAM_MACRO,         pot_s);
        g_state.set_patch_param(PARAM_TONAL,         pot_s);
        g_state.set_patch_param(PARAM_FILTER_MACRO,  clamp01(0.20f+pot_s*0.70f));
        g_state.set_patch_param(PARAM_MORPH,         pot_s);
        g_state.set_bus_param(PARAM_REVERB_WET,      clamp01(0.05f+pot_s*0.20f));
        scan_pads();
        if (pad_on[0]&&!pad_prev[0]) { randomize_engine(pot_s); push_event(EVT_PAD_TRIGGER,0,1.0f); gpio_put(PIN_LED,1); }
        if (!pad_on[0]) gpio_put(PIN_LED,0);
        if (pad_on[1]&&!pad_prev[1]) push_event(EVT_DRUM_HIT,DRUM_KICK,  1.0f);
        if (pad_on[2]&&!pad_prev[2]) push_event(EVT_DRUM_HIT,DRUM_SNARE, 1.0f);
        if (pad_on[3]&&!pad_prev[3]) push_event(EVT_DRUM_HIT,DRUM_HAT,   1.0f);
    }
}
#endif // WITH_CONTROLS

int main() {
    set_sys_clock_khz(125000, true);
    stdio_init_all();
    sleep_ms(200);
    LOG("=== Dem0!Byt3 V1 ===");

    g_state.init();

    // Parámetros iniciales — fórmulas conocidas que producen sonido
    g_state.set_patch_param(PARAM_FORMULA_A,    0.12f);
    g_state.set_patch_param(PARAM_FORMULA_B,    0.60f);
    g_state.set_patch_param(PARAM_RATE,         0.50f);
    g_state.set_patch_param(PARAM_SHIFT,        0.35f);
    g_state.set_patch_param(PARAM_MASK,         0.65f);
    g_state.set_patch_param(PARAM_FEEDBACK,     0.20f);
    g_state.set_patch_param(PARAM_FILTER_MACRO, 0.50f);
    g_state.set_patch_param(PARAM_MACRO,        0.50f);
    g_state.set_patch_param(PARAM_TONAL,        0.50f);
    g_state.set_patch_param(PARAM_MORPH,        0.50f);
    g_state.set_patch_param(PARAM_ENV_MACRO,    0.90f);
    g_state.set_patch_param(PARAM_ENV_ATTACK,   0.02f);
    g_state.set_patch_param(PARAM_ENV_RELEASE,  0.80f);
    g_state.set_bus_param(PARAM_REVERB_ROOM,    0.55f);
    g_state.set_bus_param(PARAM_REVERB_WET,     0.12f);
    g_state.set_bus_param(PARAM_DRUM_DECAY,     0.50f);
    g_state.set_bus_param(PARAM_DRUM_COLOR,     0.50f);
    g_state.set_bus_param(PARAM_DUCK_AMOUNT,    0.80f);
    g_state.set_env_loop(true);

    g_audio_engine.init(&g_audio_out, &g_state);
    g_audio_engine.set_event_queue(&g_event_queue);

#ifdef WITH_CONTROLS
    multicore_lockout_victim_init();
    multicore_launch_core1(core1_main);
    while (!g_ready) sleep_ms(10);
    LOG("Core0: pads calibrados, entrando en audio loop");
#else
    LOG("Core0: modo sin controles, entrando en audio loop");
#endif

    // Tight loop — pio_sm_put_blocking sincroniza al sample rate del DAC
    while (true) {
        g_audio_engine.process_one_sample();
    }
}
