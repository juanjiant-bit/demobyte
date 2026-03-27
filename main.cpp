
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

static bool g_drone = true;

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

int main() {
    stdio_init_all();

    controls::init();
    g_i2s.init();
    g_synth.init();
    g_drums.init();
    g_master.init();
    g_synth.set_drone(g_drone);

    // Cada encendido arranca con fórmulas nuevas.
    g_synth.randomize_on_boot();

    absolute_time_t last = get_absolute_time();

    // Trigger derivado desde pressure mientras termina de resolverse la capa de pads.
    bool prev_gate[4] = {false, false, false, false};
    constexpr float kTrigPressure = 0.16f;
    constexpr float kRelPressure  = 0.08f;

    while (true) {
        const absolute_time_t now = get_absolute_time();
        if (absolute_time_diff_us(last, now) >= 1000) {
            last = now;
            controls::update_1ms();

            const auto& p1 = controls::pad(0);
            const auto& p2 = controls::pad(1);
            const auto& p3 = controls::pad(2);
            const auto& p4 = controls::pad(3);

            const bool gate1 = p1.pressure > kTrigPressure;
            const bool gate2 = p2.pressure > kTrigPressure;
            const bool gate3 = p3.pressure > kTrigPressure;
            const bool gate4 = p4.pressure > kTrigPressure;

            const bool trig1 = gate1 && !prev_gate[0];
            const bool trig2 = gate2 && !prev_gate[1];
            const bool trig3 = gate3 && !prev_gate[2];
            const bool trig4 = gate4 && !prev_gate[3];

            if (p1.pressure < kRelPressure) prev_gate[0] = false; else prev_gate[0] = gate1;
            if (p2.pressure < kRelPressure) prev_gate[1] = false; else prev_gate[1] = gate2;
            if (p3.pressure < kRelPressure) prev_gate[2] = false; else prev_gate[2] = gate3;
            if (p4.pressure < kRelPressure) prev_gate[3] = false; else prev_gate[3] = gate4;

            if (trig1) {
                g_drone = !g_drone;
                g_synth.set_drone(g_drone);
                g_synth.next_formula_pair();
            }
            if (trig2) g_drums.trigger_kick();
            if (trig3) g_drums.trigger_snare();
            if (trig4) g_drums.trigger_hat();

            // Volumen y morph quedan igual
            g_synth.set_morph(controls::morph());

            // Aftertouch del pad 1 pasa a ser el "color vivo" / tape-rate.
            const float at = clamp01(p1.pressure);
            const float live_color = clamp01(1.0f - 0.92f * at);
            g_synth.set_color(live_color);
            g_synth.set_pressure(at);

            // El antiguo pote color ahora pasa a "mod": cuerpo + complejidad.
            g_synth.set_mod(controls::color());

            g_master.set_volume(controls::volume());
        }

        const float mod = controls::color();
        float bb = g_synth.render();
        const float duck = 1.0f - 0.55f * g_drums.kick_env();
        float drum = g_drums.render(mod);

        float mix = bb * duck * 0.96f + drum * 0.92f;
        mix = g_master.process(mix);

        const int16_t s = f_to_i16(mix);
        g_i2s.write(s, s);
    }
}
