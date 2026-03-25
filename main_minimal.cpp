// main_minimal.cpp — Dem0!Byt3 V1
//
// DIAGNÓSTICO BINARIO EN 3 ETAPAS:
//
// ETAPA 1 (activa): Square wave hardcodeada directo al PIO
//   Si suena → hardware OK, el problema estaba en AudioEngine
//   Si no suena → problema de hardware (XSMT, wiring, PIO)
//
// ETAPA 2 (comentada): AudioEngine puro sin StateManager
//   Descomentar #define STAGE2 para activar
//
// ETAPA 3 (comentada): AudioEngine + StateManager completo
//   Descomentar #define STAGE3 para activar

// #define STAGE2
// #define STAGE3

#include <cstdint>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "audio/pcm5102_i2s.pio.h"

#if defined(STAGE2) || defined(STAGE3)
#include "pico/multicore.h"
#include "hardware/adc.h"
#include "audio/audio_engine.h"
#include "audio/audio_output_i2s.h"
#include "state/state_manager.h"
#include "utils/ring_buffer.h"
#include "sequencer/event_types.h"
#endif

#include "utils/debug_log.h"

// ── Pines I2S ─────────────────────────────────────────────────────
constexpr uint PIN_BCLK = 10;
constexpr uint PIN_DIN  = 12;
constexpr uint PIN_LED  = 25;

// ── ETAPA 1: Square wave hardcodeada ─────────────────────────────
#if !defined(STAGE2) && !defined(STAGE3)

int main() {
    set_sys_clock_khz(125000, true);
    stdio_init_all();
    sleep_ms(200);
    LOG("=== Dem0!Byt3 V1 STAGE1: square wave directo al PIO ===");

    gpio_init(PIN_LED); gpio_set_dir(PIN_LED, GPIO_OUT);

    PIO pio = pio0;
    uint offset = pio_add_program(pio, &pcm5102_i2s_program);
    uint sm = pio_claim_unused_sm(pio, true);
    pcm5102_i2s_program_init(pio, sm, offset, PIN_DIN, PIN_BCLK, 44100);

    // Precargar silencio
    for (int i = 0; i < 32; ++i) {
        pio_sm_put_blocking(pio, sm, 0);
        pio_sm_put_blocking(pio, sm, 0);
    }

    LOG("PIO inicializado, generando 440Hz square wave...");
    gpio_put(PIN_LED, 1);

    // 440Hz square wave: periodo = 44100/440 = 100.2 samples
    // Mitad HIGH, mitad LOW = 50 samples cada una
    uint32_t t = 0;
    while (true) {
        // Square wave 440Hz: positivo primero 50 samples, negativo 50 samples
        const int16_t sample = ((t % 100) < 50) ? 16000 : -16000;
        const uint32_t slot = (uint32_t)(uint16_t)sample << 16;
        pio_sm_put_blocking(pio, sm, slot);  // L
        pio_sm_put_blocking(pio, sm, slot);  // R
        ++t;
    }
}

// ── ETAPA 2: AudioEngine sin StateManager ─────────────────────────
#elif defined(STAGE2)

static AudioOutputI2S g_audio_out;
static AudioEngine    g_audio_engine;

int main() {
    set_sys_clock_khz(125000, true);
    stdio_init_all();
    sleep_ms(200);
    LOG("=== Dem0!Byt3 V1 STAGE2: AudioEngine sin StateManager ===");

    gpio_init(PIN_LED); gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 1);

    // Sin StateManager: init(output, nullptr)
    // process_one_sample() ve state_mgr_==nullptr → output_->write(0,0)
    // Esto NO produce sonido pero valida que el PIO funciona desde AudioEngine
    g_audio_engine.init(&g_audio_out, nullptr);

    LOG("AudioEngine inicializado (sin StateManager), tight loop...");

    while (true) {
        g_audio_engine.process_one_sample();
    }
}

// ── ETAPA 3: AudioEngine + StateManager completo ──────────────────
#else

static RingBuffer<SequencerEvent, 128> g_event_queue;
static StateManager   g_state;
static AudioOutputI2S g_audio_out;
static AudioEngine    g_audio_engine;

int main() {
    set_sys_clock_khz(125000, true);
    stdio_init_all();
    sleep_ms(200);
    LOG("=== Dem0!Byt3 V1 STAGE3: AudioEngine + StateManager ===");

    gpio_init(PIN_LED); gpio_set_dir(PIN_LED, GPIO_OUT);

    g_state.init();
    g_state.set_patch_param(PARAM_FORMULA_A,    0.12f);
    g_state.set_patch_param(PARAM_FORMULA_B,    0.60f);
    g_state.set_patch_param(PARAM_RATE,         0.50f);
    g_state.set_patch_param(PARAM_FILTER_MACRO, 0.50f);
    g_state.set_patch_param(PARAM_MACRO,        0.50f);
    g_state.set_patch_param(PARAM_MORPH,        0.50f);
    g_state.set_patch_param(PARAM_ENV_MACRO,    0.90f);
    g_state.set_patch_param(PARAM_ENV_ATTACK,   0.02f);
    g_state.set_patch_param(PARAM_ENV_RELEASE,  0.80f);
    g_state.set_bus_param(PARAM_REVERB_WET,     0.00f);  // sin reverb para test
    g_state.set_bus_param(PARAM_DUCK_AMOUNT,    0.00f);  // sin duck
    g_state.set_env_loop(true);

    g_audio_engine.init(&g_audio_out, &g_state);
    g_audio_engine.set_event_queue(&g_event_queue);

    // Startup trigger: activa el AR envelope desde el primer sample
    {
        SequencerEvent ev{};
        ev.type = EVT_PAD_TRIGGER; ev.target = 0; ev.value = 1.0f;
        g_event_queue.push(ev);
    }

    gpio_put(PIN_LED, 1);
    LOG("Entrando en audio loop...");

    while (true) {
        g_audio_engine.process_one_sample();
    }
}

#endif
