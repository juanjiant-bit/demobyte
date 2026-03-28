
#include "pico/stdlib.h"
#include "audio/audio_output_i2s.h"
#include "io/pads.h"
#include "synth/bytebeat_engine.h"
#include "drums/drum_engine.h"
#include "master/master.h"

static audio::AudioOutputI2S g_i2s;
static synth::BytebeatEngine g_synth;
static drums::DrumEngine g_drums;
static master::Master g_master;

static inline int16_t f_to_i16(float x) {
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    return static_cast<int16_t>(x * 32767.0f);
}

static inline float clamp01(float x) {
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

// IMPORTANTE:
// En la integración anterior el trigger usaba max(raw_metric, pressure),
// y eso deja puerta a falsos disparos porque pressure sigue viva un rato.
// Acá el TRIGGER sale SOLO de raw.
// El aftertouch sigue viniendo SOLO de pressure.
static inline float raw_gate_metric(const controls::PadState& p) {
    // Reposo medido: ~19..25
    // Toque suave: ~100
    // Dejamos una zona muerta amplia para evitar fantasmas.
    if (p.raw <= 40) return 0.0f;

    float m = float(int(p.raw) - 40) / 60.0f; // 40..100 -> 0..1
    if (m < 0.0f) m = 0.0f;
    if (m > 1.0f) m = 1.0f;
    return m;
}

int main() {
    stdio_init_all();
    sleep_ms(1200);   // igual que en el pad-test que sí funcionó

    controls::init();
    g_i2s.init();
    g_synth.init();
    g_drums.init();
    g_master.init();
    g_synth.set_drone(true);

    absolute_time_t last = get_absolute_time();

    bool prev_gate[4] = {false, false, false, false};
    uint32_t lockout_ms[4] = {0, 0, 0, 0};

    constexpr float kTrig = 0.18f;
    constexpr float kRel  = 0.07f;
    constexpr uint32_t kLockout = 110;

    // ignorar triggers en el arranque mientras todo se estabiliza
    const uint32_t boot_ignore_until = to_ms_since_boot(get_absolute_time()) + 1500;

    while (true) {
        const absolute_time_t now = get_absolute_time();
        if (absolute_time_diff_us(last, now) >= 1000) {
            last = now;
            controls::update_1ms();

            const uint32_t now_ms = to_ms_since_boot(now);

            const auto& p1 = controls::pad(0);
            const auto& p2 = controls::pad(1);
            const auto& p3 = controls::pad(2);
            const auto& p4 = controls::pad(3);

            const float m[4] = {
                raw_gate_metric(p1),
                raw_gate_metric(p2),
                raw_gate_metric(p3),
                raw_gate_metric(p4)
            };

            for (int i = 0; i < 4; ++i) {
                const bool gate = m[i] > kTrig;
                bool trig = false;

                if (now_ms > boot_ignore_until && now_ms > lockout_ms[i]) {
                    trig = gate && !prev_gate[i];
                }

                if (trig) {
                    lockout_ms[i] = now_ms + kLockout;

                    if (i == 0) {
                        // SOLO random fórmula. No toggle de drone.
                        g_synth.next_formula_pair();
                    } else if (i == 1) {
                        g_drums.trigger_kick();
                    } else if (i == 2) {
                        g_drums.trigger_snare();
                    } else if (i == 3) {
                        g_drums.trigger_hat();
                    }
                }

                prev_gate[i] = (m[i] > kRel) ? gate : false;
            }

            // Controles originales de BUENA
            g_synth.set_morph(controls::morph());
            g_synth.set_color(controls::color());

            // Aftertouch expresivo SOLO desde pressure del pad 1
            g_synth.set_pressure(clamp01(p1.pressure));

            g_master.set_volume(controls::volume());
        }

        const float color = controls::color();
        float bb = g_synth.render();
        const float duck = 1.0f - 0.55f * g_drums.kick_env();
        float drum = g_drums.render(color);

        float mix = bb * duck * 0.96f + drum * 0.92f;
        mix = g_master.process(mix);

        const int16_t s = f_to_i16(mix);
        g_i2s.write(s, s);
    }
}
