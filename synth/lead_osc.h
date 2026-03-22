#pragma once
// lead_osc.h — Bytebeat Machine V1.21
// Oscilador senoidal con modulación FM/AM por el bytebeat activo.
//
// MODULACIÓN (Note Mode):
//   FM: el output del bytebeat del frame anterior modula el delta de fase.
//       fm_depth=0.0 → seno puro, fm_depth=1.0 → timbre bytebeat impreso.
//   AM: el bytebeat modula la amplitud del seno (complemento sutil del FM).
//   set_mod_depth(macro): macro 0.0–1.0 controla ambas profundidades.
//     macro=0 → seno puro afinado
//     macro=1 → FM máximo, el bytebeat tiñe la afinación completamente
//
// PORTAMENTO: glide entre frecuencias por slew rate en set_freq_slew().
//
#include <cstdint>
#include "bytebeat_node.h"  // SINE_TABLE está en drum_engine.h pero
                            // para no duplicar usamos la del drum

// Tabla seno Q15 256 puntos (definida en drum_engine.h como extern o inline)
// Para evitar duplicados, declaramos extern y la definimos en un .cpp
extern const int16_t SINE_TABLE_256[256];

class LeadOsc {
public:
    void init() {
        phase_q24_   = 0;
        freq_hz_     = 220.0f;
        target_freq_ = 220.0f;
        delta_       = freq_to_delta(220.0f);
        target_delta_= delta_;
        fm_depth_    = 0.0f;
        am_depth_    = 0.0f;
        fm_q12_      = 0;
        am_q14_      = 0;
        bb_q15_      = 0;
    }

    // Llamar cuando cambia la frecuencia (no necesariamente cada sample)
    void set_freq(float hz) {
        if (hz < 20.0f)   hz = 20.0f;
        if (hz > 8000.0f) hz = 8000.0f;
        freq_hz_ = hz;
        target_freq_ = hz;
        delta_   = freq_to_delta(hz);
        target_delta_ = delta_;
        slew_active_ = false;
    }

    // Portamento suave: slew hacia target_freq en ~20ms
    void set_freq_slew(float hz) {
        if (hz < 20.0f)   hz = 20.0f;
        if (hz > 8000.0f) hz = 8000.0f;
        target_freq_ = hz;
        target_delta_ = freq_to_delta(hz);
        slew_active_ = true;
    }

    // Alimentar el output bytebeat normalizado del frame anterior (-1.0..+1.0).
    // Llamar una vez por sample ANTES de process().
    // Si mod_depth=0 esta llamada no tiene efecto en el audio.
    void feed_bytebeat(int16_t bb_sample) {
        // Slew Q15 barato en hot path; evita float por sample.
        const int32_t target_q15 = ((int32_t)bb_sample << 15) / 32767;
        bb_q15_ += (int32_t)((target_q15 - bb_q15_) * 2621) >> 15;  // ~0.08
    }

    // Profundidad de modulación FM+AM desde el parámetro Macro (0.0–1.0).
    // FM depth: 0.0–0.40 (saturado en 0.4 para evitar aliasing excesivo).
    // AM depth: 0.0–0.15 (complemento sutil, 37.5% del FM).
    // Llamar cuando cambia el macro (control-rate, no cada sample).
    void set_mod_depth(float macro) {
        const float m = macro < 0.0f ? 0.0f : (macro > 1.0f ? 1.0f : macro);
        fm_depth_ = m * 0.40f;
        am_depth_ = m * 0.15f;
        fm_q12_ = (int32_t)(fm_depth_ * 4096.0f);   // 0..~1638
        am_q14_ = (int32_t)(am_depth_ * 16384.0f);  // 0..~2458
    }

    // process(): llamar cada sample. Retorna int16_t.
    // Aplica FM/AM si fm_depth_ > 0.
    int16_t process() {
        // Portamento con delta Q24: evita float por sample.
        if (slew_active_) {
            int32_t diff = (int32_t)target_delta_ - (int32_t)delta_;
            if (diff > 8 || diff < -8) {
                int32_t step = diff >> 9;  // ~constante de 512 samples
                if (step == 0) step = (diff > 0) ? 1 : -1;
                delta_ = (uint32_t)((int32_t)delta_ + step);
            } else {
                delta_ = target_delta_;
                freq_hz_ = target_freq_;
                slew_active_ = false;
            }
        }

        uint32_t delta_mod = delta_;
        if (fm_q12_ > 0) {
            int32_t mod_q12 = 4096 + ((fm_q12_ * bb_q15_) >> 15);
            if (mod_q12 < 410) mod_q12 = 410;       // ~0.1x
            if (mod_q12 > 12288) mod_q12 = 12288;   // 3.0x
            delta_mod = (uint32_t)(((uint64_t)delta_ * (uint32_t)mod_q12) >> 12);
        }

        phase_q24_ += delta_mod;
        if (phase_q24_ >= 0x1000000u) phase_q24_ -= 0x1000000u;

        const uint8_t idx = (uint8_t)(phase_q24_ >> 16);
        int16_t s = SINE_TABLE_256[idx];

        if (am_q14_ > 0) {
            int32_t am_q14 = 16384 + ((am_q14_ * bb_q15_) >> 15);
            if (am_q14 < 0) am_q14 = 0;
            if (am_q14 > 32768) am_q14 = 32768;
            s = (int16_t)(((int32_t)s * am_q14) >> 14);
        }

        return s;
    }

    float get_freq() const { return freq_hz_; }

private:
    static uint32_t freq_to_delta(float hz) {
        return (uint32_t)(hz * 380.633f);
    }

    static constexpr float SLEW_ALPHA = 0.002f;

    uint32_t phase_q24_   = 0;
    uint32_t delta_       = 0;
    uint32_t target_delta_= 0;
    float    freq_hz_     = 220.0f;
    float    target_freq_ = 220.0f;
    bool     slew_active_ = false;

    // Modulación FM/AM
    float    fm_depth_    = 0.0f;   // mantenido para compatibilidad/telemetría
    float    am_depth_    = 0.0f;
    int32_t  fm_q12_      = 0;
    int32_t  am_q14_      = 0;
    int32_t  bb_q15_      = 0;      // bytebeat suavizado Q15
};
