// main_diag.cpp — BYT3 audio diagnostic V1.0
//
// Cambiá DIAG_MODE para aislar el problema:
//   MODE 1: bytebeat puro en tight loop — sin StateManager, sin timer, sin Core1
//           1 blink al arrancar. Si NO suena: problema de PIO/hardware.
//           Si SÍ suena: el hardware I2S funciona.
//
//   MODE 2: modo 1 + ADC. Pot cambia el pitch del bytebeat.
//           2 blinks al arrancar. LED parpadea más rápido con pot alto.
//           Si pitch no cambia: problema con ADC.
//
//   MODE 3: AudioEngine + StateManager sin Core1.
//           Fórmula fija, sin FX. 3 blinks al arrancar. LED fijo = corriendo.
//           Si modo 1/2 suenan pero modo 3 no: problema en AudioEngine.

#define DIAG_MODE 1   // <-- CAMBIAR: 1, 2, o 3

#include <cstdint>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pcm5102_i2s.pio.h"

static constexpr uint PIN_BCLK = 10;
static constexpr uint PIN_DIN  = 12;
static constexpr uint PIN_LED  = 25;
static constexpr uint PIN_POT  = 26;
static constexpr uint SAMPLE_RATE = 44100;

static PIO  g_pio = pio0;
static uint g_sm  = 0;

static inline void i2s_write(int16_t l, int16_t r) {
    pio_sm_put_blocking(g_pio, g_sm, (uint32_t)(uint16_t)l << 16);
    pio_sm_put_blocking(g_pio, g_sm, (uint32_t)(uint16_t)r << 16);
}

static void i2s_init() {
    const uint offset = pio_add_program(g_pio, &pcm5102_i2s_program);
    g_sm = pio_claim_unused_sm(g_pio, true);
    pcm5102_i2s_program_init(g_pio, g_sm, offset, PIN_DIN, PIN_BCLK, SAMPLE_RATE);
    for (int i = 0; i < 32; ++i) i2s_write(0, 0);
}

static void blink(int n) {
    for (int i = 0; i < n * 2; i++) {
        gpio_put(PIN_LED, i & 1);
        sleep_ms(120);
    }
    sleep_ms(300);
}

// Bytebeat clásico — siempre produce sonido audible
static inline int16_t bytebeat(uint32_t t, uint32_t rate_mul = 256) {
    const uint32_t ts = (t * rate_mul) >> 8;
    const uint8_t b = (uint8_t)(((ts >> 10) & 42) * ts);
    return (int16_t)((int)(b) - 128) << 7;
}

#if DIAG_MODE == 1
// ── MODE 1: tight loop puro ───────────────────────────────────────
int main() {
    set_sys_clock_khz(125000, true);
    gpio_init(PIN_LED); gpio_set_dir(PIN_LED, GPIO_OUT);
    i2s_init();
    blink(1);
    gpio_put(PIN_LED, 1);
    uint32_t t = 0;
    while (true) { i2s_write(bytebeat(t), bytebeat(t)); ++t; }
}

#elif DIAG_MODE == 2
// ── MODE 2: tight loop + ADC ─────────────────────────────────────
int main() {
    set_sys_clock_khz(125000, true);
    gpio_init(PIN_LED); gpio_set_dir(PIN_LED, GPIO_OUT);
    i2s_init();
    adc_init(); adc_gpio_init(PIN_POT); adc_select_input(0);
    blink(2);
    uint32_t t = 0, pot_ctr = 0, rate_mul = 256;
    while (true) {
        if (++pot_ctr >= 2000) {
            pot_ctr = 0;
            const float pot = float(adc_read()) * (1.0f / 4095.0f);
            rate_mul = (uint32_t)((0.25f + pot * 3.75f) * 256.0f);
            gpio_put(PIN_LED, (t >> (int)(12 - pot * 6)) & 1);
        }
        i2s_write(bytebeat(t, rate_mul), bytebeat(t, rate_mul));
        ++t;
    }
}

#elif DIAG_MODE == 3
// ── MODE 3: AudioEngine + StateManager, sin Core1 ────────────────
#include "pico/multicore.h"
#include "audio/audio_engine.h"
#include "audio/audio_output_i2s.h"
#include "state/state_manager.h"
#include "utils/ring_buffer.h"
#include "sequencer/event_types.h"

static RingBuffer<SequencerEvent, 128> g_queue;
static StateManager   g_state;
static AudioOutputI2S g_audio_out;
static AudioEngine    g_audio_engine;

int main() {
    set_sys_clock_khz(125000, true);
    stdio_init_all();
    gpio_init(PIN_LED); gpio_set_dir(PIN_LED, GPIO_OUT);
    blink(3);

    g_state.init();
    g_state.set_env_loop(true);
    g_state.set_patch_param(PARAM_ENV_ATTACK,  0.01f);
    g_state.set_patch_param(PARAM_ENV_RELEASE, 0.30f);
    g_state.set_patch_param(PARAM_MACRO,       0.60f);
    g_state.set_patch_param(PARAM_TONAL,       0.50f);
    g_state.set_patch_param(PARAM_FORMULA_A,   0.00f);
    g_state.set_patch_param(PARAM_FORMULA_B,   0.50f);
    g_state.set_patch_param(PARAM_MORPH,       0.50f);
    g_state.set_patch_param(PARAM_RATE,        0.55f);
    g_state.set_patch_param(PARAM_MASK,        0.75f);
    g_state.set_patch_param(PARAM_FEEDBACK,    0.20f);
    g_state.set_bus_param(PARAM_REVERB_WET,    0.0f);
    g_state.set_bus_param(PARAM_CHORUS,        0.0f);
    g_state.set_bus_param(PARAM_DELAY_WET,     0.0f);
    g_state.set_bus_param(PARAM_DUCK_AMOUNT,   0.0f);

    SequencerEvent ev{}; ev.type = EVT_PAD_TRIGGER; ev.value = 1.0f;
    g_queue.push(ev);

    g_audio_engine.init(&g_audio_out, &g_state);
    g_audio_engine.set_event_queue(&g_queue);

    gpio_put(PIN_LED, 1);
    g_audio_engine.run();
}
#endif
