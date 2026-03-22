#pragma once
// exciter.h — Harmonic Exciter / Stage1 Sound Depth Upgrade
//
// Agrega presencia de alta frecuencia al bytebeat mediante saturación
// armónica paralela en la banda alta. No es más volumen — es más "aire"
// y definición: el sonido "sale" del mix.
//
// Algoritmo (clásico Aphex Aural Exciter simplificado):
//   1. HP 1-polo → aislar banda alta (fc configurable, default 2.5kHz)
//   2. Saturación suave (tanh cúbico) → generar armónicos pares e impares
//   3. Mezcla paralela: dry + (sat × amount) → el dry nunca se toca
//
// Posición en DspChain: después del soft clip (paso 5), antes del chorus (paso 6).
// Así actúa sobre la señal ya limitada y antes del procesamiento espacial.
//
// Parámetros:
//   amount  0.0–1.0:
//     0.0       → bypass completo (zero cost)
//     0.0–1.0   → mezcla paralela sube; la saturación es proporcional
//   freq_hz 500–8000Hz: corte del HP interno (default 2500Hz)
//
// Control en YUY0:
//   Acoplado al drive: cuando drive > 0.4 el exciter empieza a activarse
//   (ratio 1.67× sobre el rango 0.4–1.0). Ningún pot nuevo necesario.
//
// CPU: ~6 multiplicaciones por sample (estéreo). 0 si amount=0.
// RAM: 4 × int32_t = 16 bytes (estado HP L/R + prev x L/R).
//
#include <cstdint>

struct Exciter {

    void init() {
        hp_l_     = 0;  hp_r_     = 0;
        x_prev_l_ = 0;  x_prev_r_ = 0;
        amount_   = 0.f;
        set_freq(2500.f);   // fc default: 2.5kHz
    }

    // amount [0.0, 1.0] — 0 = bypass total, 1 = excitación máxima.
    // Internamente se limita a 0.40 de mezcla paralela para no destruir
    // el carácter original; el rango perceptual es amplio dentro de ese límite.
    void set_amount(float a) {
        if (a < 0.f) a = 0.f;
        if (a > 1.f) a = 1.f;
        amount_ = a * 0.40f;   // límite interno: max +3dB de presencia
    }

    // fc del HP en Hz. Determina qué banda se excita.
    // 2000–3000Hz: brillo general y presencia
    // 3000–5000Hz: "aire" y detalle muy fino
    // <2000Hz: empieza a afectar el mid — usar con cuidado
    void set_freq(float hz) {
        if (hz <  500.f) hz =  500.f;
        if (hz > 8000.f) hz = 8000.f;
        // HP 1-polo: coeff = 1 / (1 + 2π*fc/fs)
        // Aproximación sin expf() — válida para fc < fs/6
        const float rc = 1.f / (6.2832f * hz / 44100.f);
        hp_coeff_ = rc / (rc + 1.f);
    }

    bool is_active() const { return amount_ > 0.001f; }

    void process(int16_t& L, int16_t& R) {
        if (!is_active()) return;

        // ── HP 1-polo estéreo ─────────────────────────────────────────────
        // y[n] = coeff * (y[n-1] + x[n] - x[n-1])
        // Implementación en float para precisión en la banda alta.
        const float fl = static_cast<float>(L) * (1.f / 32768.f);
        const float fr = static_cast<float>(R) * (1.f / 32768.f);

        const float hp_l = hp_coeff_ * (hp_l_ + fl - x_prev_l_);
        const float hp_r = hp_coeff_ * (hp_r_ + fr - x_prev_r_);

        // Guardar estado (sin clamp — el HP está bien condicionado)
        hp_l_     = hp_l;
        hp_r_     = hp_r;
        x_prev_l_ = fl;
        x_prev_r_ = fr;

        // ── Saturación armónica (tanh cúbico) ────────────────────────────
        // tanh(x) ≈ x - x³/3  para |x| ≤ 1 → válido aquí ya que hp ∈ [-1, 1]
        // Genera armónicos pares (2°) e impares (3°) sin aliasing perceptible
        // porque sólo actúa sobre la banda alta (ya filtrada por el HP).
        const float sat_l = hp_l - (hp_l * hp_l * hp_l) * (1.f / 3.f);
        const float sat_r = hp_r - (hp_r * hp_r * hp_r) * (1.f / 3.f);

        // ── Mezcla paralela ───────────────────────────────────────────────
        // El dry siempre está presente en su totalidad.
        // La saturación se suma a nivel amount_ (0..0.40).
        const float out_l = fl + sat_l * amount_;
        const float out_r = fr + sat_r * amount_;

        // Convertir de vuelta a int16_t con clamp
        const int32_t il = static_cast<int32_t>(out_l * 32767.f);
        const int32_t ir = static_cast<int32_t>(out_r * 32767.f);

        L = static_cast<int16_t>(il >  32767 ?  32767 : (il < -32768 ? -32768 : il));
        R = static_cast<int16_t>(ir >  32767 ?  32767 : (ir < -32768 ? -32768 : ir));
    }

private:
    // Estado HP
    float hp_l_     = 0.f;
    float hp_r_     = 0.f;
    float x_prev_l_ = 0.f;
    float x_prev_r_ = 0.f;
    // Parámetros
    float amount_   = 0.f;
    float hp_coeff_ = 0.f;
};
