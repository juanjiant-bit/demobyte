// pads.cpp — medición resistiva por tiempo real
// Circuito: 3V3 → 100kΩ → GPIO_PIN, 100nF entre GPIO_PIN y GND
//
// Técnica: OUTPUT LOW (descarga cap) → INPUT (suelta) → mide µs hasta HIGH
//   Sin dedo: cap carga a 3V3 por 100kΩ, HIGH en ~9.3ms (τ=10ms)
//   Con dedo: R_skin en paralelo → más lento o nunca llega → TOQUE
//
// Threshold: tiempo medido > baseline×1.20 → toque
// Presión (aftertouch): qué tan ARRIBA del threshold (0..1)
//
// NOTA: la versión anterior usaba conteo de loops (kMaxCount=2200 = ~22µs)
//   que es 400× más corto que el RC=10ms → nunca detectaba → FIXED

#include "io/pads.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include <algorithm>
#include <cmath>

namespace controls {
namespace {

PadState  g_pads[kNumPads];
PotState  g_pots[kNumPots];

// ── Constantes de detección ───────────────────────────────────────
// Con 100kΩ + 100nF:
//   Sin dedo: HIGH en ~9300µs
//   Con dedo R_skin=200kΩ: HIGH en ~16000µs → ratio 1.72
//   Con dedo R_skin=500kΩ: HIGH en ~10800µs → ratio 1.16
//   Con dedo R_skin<100kΩ: NUNCA llega (V_ss < VIH) → timeout

constexpr uint32_t PAD_DISCHARGE_US = 800;   // descarga el cap antes de medir
constexpr uint32_t PAD_TIMEOUT_US   = 14000; // 14ms — bien por encima del baseline

// Threshold para activar: 20% más lento que el baseline calibrado
constexpr float kTouchRatio  = 1.20f;
// Threshold para soltar: 8% más lento (histéresis)
constexpr float kReleaseRatio = 1.08f;

// Para presión analógica: mapea entre kTouchRatio y kMaxRatio
constexpr float kMaxRatio    = 2.50f;  // timeout = presión máxima

constexpr uint8_t kConfirmOn  = 3;  // lecturas consecutivas para activar
constexpr uint8_t kConfirmOff = 2;  // lecturas consecutivas para soltar

// ── Medición de un pad por tiempo real ───────────────────────────
uint32_t measure_pad_us(uint pin){
    // 1. Descarga el cap rápido (OUTPUT LOW)
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 0);
    sleep_us(PAD_DISCHARGE_US);

    // 2. Suelta el pin — el 100kΩ externo carga el cap hacia 3V3
    gpio_set_dir(pin, GPIO_IN);
    gpio_disable_pulls(pin);

    // 3. Mide tiempo hasta HIGH (µs)
    const uint32_t t0 = time_us_32();
    while(!gpio_get(pin)){
        if((time_us_32()-t0) >= PAD_TIMEOUT_US)
            return PAD_TIMEOUT_US;
    }
    return time_us_32()-t0;
}

// Calibración: promedio de 20 muestras con outliers descartados
uint32_t calibrate_pad(uint pin){
    uint32_t vals[20];
    for(int i=0;i<20;++i){
        vals[i] = measure_pad_us(pin);
        sleep_us(200);
    }
    // Descartar 4 más altos y 4 más bajos → promedio del centro
    std::sort(vals, vals+20);
    uint64_t sum=0;
    for(int i=4;i<16;++i) sum+=vals[i];
    return (uint32_t)(sum/12);
}

// ── ADC helpers ───────────────────────────────────────────────────
uint16_t read_adc_avg(uint adc_input){
    adc_select_input(adc_input);
    uint32_t sum=0;
    for(int i=0;i<8;++i) sum+=adc_read();
    return (uint16_t)(sum/8);
}

float smooth_pot(int idx, float target, float alpha){
    g_pots[idx].value += alpha*(target-g_pots[idx].value);
    float diff = fabsf(g_pots[idx].value - g_pots[idx].stable);
    if(diff > 0.0025f) g_pots[idx].stable = g_pots[idx].value;
    return g_pots[idx].stable;
}

} // namespace

// ── Init ─────────────────────────────────────────────────────────
void init(){
    adc_init();
    for(int i=0;i<kNumPots;++i) adc_gpio_init(kPotPins[i]);

    // Calibrar cada pad — mide el baseline real con este hardware
    for(int i=0;i<kNumPads;++i){
        gpio_init(kPadPins[i]);
        gpio_set_dir(kPadPins[i], GPIO_IN);
        gpio_disable_pulls(kPadPins[i]);
        uint32_t bl = calibrate_pad(kPadPins[i]);
        // Sanity: si calibró en timeout (sin pullup externo), usar valor seguro
        if(bl >= PAD_TIMEOUT_US) bl = 9000;
        g_pads[i].baseline = (uint16_t)std::min(bl, (uint32_t)60000u);
        g_pads[i].raw = g_pads[i].baseline;
    }

    for(int i=0;i<kNumPots;++i){
        uint16_t raw = read_adc_avg(i);
        g_pots[i].raw = raw;
        g_pots[i].value = raw/4095.f;
        g_pots[i].stable = g_pots[i].value;
    }
}

// ── Update 1ms ───────────────────────────────────────────────────
void update_1ms(){
    // Pots
    for(int i=0;i<kNumPots;++i){
        uint16_t raw = read_adc_avg(i);
        g_pots[i].raw = raw;
        float target = (float)raw/4095.f;
        smooth_pot(i, target, i==0 ? 0.06f : 0.10f);
    }

    // Pads — medición por tiempo real
    for(int i=0;i<kNumPads;++i){
        auto& p = g_pads[i];
        const bool was_pressed = p.pressed;
        p.trigger = false;
        p.release = false;

        uint32_t raw_us = measure_pad_us(kPadPins[i]);
        p.raw = (uint16_t)std::min(raw_us, (uint32_t)65535u);

        // Baseline drift muy lento cuando está libre
        if(!was_pressed)
            p.baseline = (uint16_t)(0.9995f*p.baseline + 0.0005f*raw_us);

        float ratio = (float)raw_us / (float)p.baseline;

        // Detección con histéresis
        bool touched = (ratio >= kTouchRatio);
        bool released = (ratio < kReleaseRatio);

        if(!was_pressed){
            if(touched){
                if(++p.on_count >= kConfirmOn){
                    p.pressed = true;
                    p.trigger = true;
                    p.on_count = 0;
                    p.off_count = 0;
                }
            } else {
                p.on_count = 0;
            }
        } else {
            if(released){
                if(++p.off_count >= kConfirmOff){
                    p.pressed = false;
                    p.release = true;
                    p.off_count = 0;
                    p.on_count = 0;
                }
            } else {
                p.off_count = 0;
            }
        }

        // Presión analógica: 0=justo en threshold, 1=timeout completo
        if(p.pressed){
            float pr = (ratio - kTouchRatio) / (kMaxRatio - kTouchRatio);
            pr = std::clamp(pr, 0.f, 1.f);
            p.pressure += 0.18f*(pr - p.pressure);
        } else {
            p.pressure *= 0.65f;
            if(p.pressure < 0.001f) p.pressure = 0.f;
        }
    }
}

const PadState& pad(int idx){ return g_pads[idx]; }
float volume() { return std::clamp(g_pots[0].stable, 0.f, 1.f)*0.97f+0.03f; }
float morph()  { return std::clamp(g_pots[1].stable, 0.f, 1.f); }
float color()  { return std::clamp(g_pots[2].stable, 0.f, 1.f); }

} // namespace controls
