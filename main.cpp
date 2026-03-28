
#include "pico/stdlib.h"
#include "audio/audio_output_i2s.h"
#include "io/pads.h"
#include "synth/bytebeat_engine.h"
#include "drums/drum_engine.h"
#include "master/master.h"
#include <cmath>

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

static inline float raw_gate_metric(const controls::PadState& p) {
    if (p.raw <= 40) return 0.0f;
    float m = float(int(p.raw) - 40) / 60.0f;
    if (m < 0.0f) m = 0.0f;
    if (m > 1.0f) m = 1.0f;
    return m;
}

int main() {
    stdio_init_all();
    sleep_ms(1200);

    controls::init();
    g_i2s.init();
    g_synth.init();
    g_drums.init();
    g_master.init();
    g_synth.set_drone(true);

    absolute_time_t last = get_absolute_time();

    bool prev_gate[4] = {false, false, false, false};
    uint32_t lockout_ms[4] = {0, 0, 0, 0};

    const float trig_th[4] = {0.18f, 0.10f, 0.10f, 0.10f};
    const float rel_th [4] = {0.07f, 0.04f, 0.04f, 0.04f};
    const uint32_t lock_ms[4] = {110, 90, 90, 70};

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

            // CLAVE:
            // solo el pad 1 puede modificar el motor de síntesis,
            // y solo cuando es claramente el pad dominante.
            const bool pad1_is_primary =
                (m[0] > 0.18f) &&
                (m[0] > m[1] + 0.12f) &&
                (m[0] > m[2] + 0.12f) &&
                (m[0] > m[3] + 0.12f);

            for (int i = 0; i < 4; ++i) {
                const bool gate = m[i] > trig_th[i];
                bool trig = false;

                if (now_ms > boot_ignore_until && now_ms > lockout_ms[i]) {
                    trig = gate && !prev_gate[i];
                }

                if (trig) {
                    lockout_ms[i] = now_ms + lock_ms[i];

                    if (i == 0) {
                        if (pad1_is_primary) {
                            g_synth.next_formula_pair();
                        }
                    } else if (i == 1) {
                        g_drums.trigger_kick();
                    } else if (i == 2) {
                        g_drums.trigger_snare();
                    } else if (i == 3) {
                        g_drums.trigger_hat();
                    }
                }

                prev_gate[i] = (m[i] > rel_th[i]) ? gate : false;
            }

            g_synth.set_morph(controls::morph());

            // Más rango útil para el pote de mod:
            // se abre el recorrido bajo y medio sin salir de 0..1.
            const float mod_wide = powf(clamp01(controls::color()), 0.22f);
            g_synth.set_color(mod_wide);

            // El aftertouch solo entra cuando realmente está activo el pad 1.
            const float p1_after = pad1_is_primary ? clamp01(p1.pressure) : 0.0f;
            g_synth.set_pressure(p1_after);

            // El volumen solo afecta al motor de síntesis.
            g_master.set_volume(1.0f);
        }

        const float synth_vol = controls::volume();
        const float drum_color = 0.25f + 0.75f * controls::color();

        float bb = g_synth.render();
        bb *= synth_vol;

        const float duck = 1.0f - 0.50f * g_drums.kick_env();
        float drum = g_drums.render(drum_color);

        float mix = bb * duck * 0.94f + drum * 1.15f;
        mix = g_master.process(mix);

        const int16_t s = f_to_i16(mix);
        g_i2s.write(s, s);
    }
}
