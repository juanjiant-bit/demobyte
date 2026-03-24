#pragma once
// delay_line.h — BYT3 V1.17
//
// Delay digital con tempo sync al BPM.
// Arquitectura send/return paralelo: dry siempre presente, wet sumado encima.
//
// TIEMPO: no es libre — siempre una división rítmica del BPM.
// El pot selecciona entre 11 divisiones escalonadas (sin interpolación continua):
//
//   Idx  División     Nombre      @ 120BPM   (negra = 500ms a 120 BPM)
//   0    1/8T         Corchea 3    166ms
//   1    1/8          Corchea      250ms
//   2    1/8D         C. dotted    375ms
//   3    1/4T         Negra 3      333ms
//   4    1/4          Negra        500ms      (default)
//   5    1/4D         N. dotted    750ms      (clamp al buffer)
//   6    1/2T         Blanca 3     667ms
//   7    1/2          Blanca      1000ms      (clamp al buffer)
//   8    1/2D         B. dotted   1500ms      (clamp al buffer)
//   9    3/4          3 tiempos   1500ms      (clamp al buffer)
//   10   1/1          Redonda     2000ms      (clamp al buffer)
//
// Buffer: int16_t, 32768 samples @ 44100Hz = ~742ms máximo sin clamp.
// Divisiones > 742ms se clampean al máximo del buffer — aún útiles como "largo".
//
// FEEDBACK: pot dedicado (mismo físico, capa SHIFT). 0.0–0.90.
// WET:      pot dedicado (mismo físico, capa SHIFT+REC). 0.0–1.0.
//
// Ping-pong estéreo: tap principal→L, tap a la mitad del tiempo→R.
// Soft clip cúbico en write path: previene overflow incluso con fb alto.
//
// RAM: 32768 × 2 bytes = 64KB (BSS estático, sin heap)
//
#include <cstdint>
#include <cstring>
#include <cmath>

static constexpr uint32_t DL_BUF_SIZE    = 32768u;   // potencia de 2
static constexpr uint32_t DL_BUF_MASK    = DL_BUF_SIZE - 1u;
static constexpr uint32_t DL_SAMPLERATE  = 44100u;
static constexpr float    DL_FB_MAX      = 0.90f;
// Mínimo en samples para evitar pitch-shifting audible
static constexpr uint32_t DL_MIN_SAMPLES = 441u;     // 10ms

// ── Tabla de divisiones rítmicas ─────────────────────────────────────────
// Expresadas como fracción de una negra (beat) en múltiplos de 1/12
// (mínimo común múltiplo para tresillos + binario + dotted).
// delay_ms = (beats * 60000) / bpm
//
// beats_x12: numerador cuando denominador = 12
//   1/8T  = 1beat * (2/3) * (1/2) = beat/3     → x12 = 4
//   1/8   = beat/2                              → x12 = 6
//   1/8D  = beat * 3/4                          → x12 = 9
//   1/4T  = beat * 2/3                          → x12 = 8
//   1/4   = 1 beat                              → x12 = 12
//   1/4D  = beat * 3/2                          → x12 = 18
//   1/2T  = beat * 4/3                          → x12 = 16
//   1/2   = 2 beats                             → x12 = 24
//   1/2D  = beat * 3                            → x12 = 36
//   3/4   = beat * 3                            → x12 = 36  (=1/2D, útil como "tres tiempos")
//   1/1   = 4 beats                             → x12 = 48

static constexpr uint8_t DL_DIV_COUNT = 11;

// beats_x12 / 12 = número de beats de negra
static constexpr uint8_t kDivBeats12[DL_DIV_COUNT] = {
    4,   // 0: 1/8T   corchea tresillo
    6,   // 1: 1/8    corchea
    9,   // 2: 1/8D   corchea con punto
    8,   // 3: 1/4T   negra tresillo
    12,  // 4: 1/4    negra  (default)
    18,  // 5: 1/4D   negra con punto
    16,  // 6: 1/2T   blanca tresillo
    24,  // 7: 1/2    blanca
    36,  // 8: 1/2D   blanca con punto
    36,  // 9: 3/4    tres tiempos (alias)
    48,  // 10: 1/1   redonda
};

// Nombres para display/debug
static constexpr const char* kDivNames[DL_DIV_COUNT] = {
    "1/8T", "1/8", "1/8.", "1/4T", "1/4", "1/4.", "1/2T", "1/2", "1/2.", "3/4", "1/1"
};

class DelayLine {
public:
    DelayLine() { clear(); }

    void clear() {
        memset(buf_, 0, sizeof(buf_));
        write_pos_  = 0;
        delay_samples_ = 22050u;  // default 500ms = 1/4 a 120 BPM (índice 4)
        feedback_   = 0.40f;
        wet_        = 0.00f;
        wet_duck_   = 1.0f;
        active_     = false;
        fb_lp_      = 0.0f;
        fb_prev_    = 0.0f;
        div_index_  = 4;          // default: 1/4 negra (índice 4)
        bpm_        = 120.0f;
    }

    // ── Parámetros ───────────────────────────────────────────────────────

    // div_norm [0..1] → selecciona una de las 11 divisiones escalonadas
    // El pot actúa como un selector de pasos: 11 zonas iguales a lo largo del recorrido
    void set_div(float div_norm) {
        if (div_norm < 0.f) div_norm = 0.f;
        if (div_norm > 1.f) div_norm = 1.f;
        const uint8_t idx = static_cast<uint8_t>(div_norm * (DL_DIV_COUNT - 1) + 0.5f);
        set_div_index(idx < DL_DIV_COUNT ? idx : DL_DIV_COUNT - 1);
    }

    // Selección directa por índice (0..DL_DIV_COUNT-1)
    void set_div_index(uint8_t idx) {
        if (idx >= DL_DIV_COUNT) idx = DL_DIV_COUNT - 1;
        div_index_ = idx;
        recalc_samples();
    }

    // BPM actual — llamar cuando cambia el encoder de BPM
    void set_bpm(float bpm) {
        if (bpm < 20.f) bpm = 20.f;
        if (bpm > 300.f) bpm = 300.f;
        bpm_ = bpm;
        recalc_samples();
    }

    void set_feedback(float fb_norm) {
        feedback_ = fb_norm * DL_FB_MAX;
        if (feedback_ < 0.f) feedback_ = 0.f;
        if (feedback_ > DL_FB_MAX) feedback_ = DL_FB_MAX;
    }

    void set_wet(float wet_norm) {
        wet_ = wet_norm;
        if (wet_ < 0.f) wet_ = 0.f;
        if (wet_ > 1.f) wet_ = 1.f;
        active_ = (wet_ > 0.001f);
    }

    void set_wet_duck(float d) {
        if (d < 0.f) d = 0.f;
        if (d > 1.f) d = 1.f;
        wet_duck_ = d;
    }

    // Fast path para audio-rate: el caller ya garantiza rango [0,1].
    inline void set_wet_duck_fast(float d) { wet_duck_ = d; }

    // ── Getters ──────────────────────────────────────────────────────────
    bool    is_active()      const { return active_; }
    uint8_t get_div_index()  const { return div_index_; }
    float   get_bpm()        const { return bpm_; }
    float   get_delay_ms()   const {
        return static_cast<float>(delay_samples_) * 1000.f / DL_SAMPLERATE;
    }
    const char* get_div_name() const { return kDivNames[div_index_]; }

    // ── Procesamiento estéreo ─────────────────────────────────────────────
    // send/return paralelo: out = dry + wet * delay_return
    // Ping-pong suave: L = tap principal, R = tap a delay/2
    void process(int16_t& inout_l, int16_t& inout_r) {
        if (!is_active()) return;

        const float in_l = static_cast<float>(inout_l) * (1.f / 32768.f);
        const float in_r = static_cast<float>(inout_r) * (1.f / 32768.f);

        // Read tap principal
        const uint32_t rpos_a = (write_pos_ - delay_samples_) & DL_BUF_MASK;
        const float tap_a = static_cast<float>(buf_[rpos_a]) * (1.f / 32768.f);

        // Read tap ping-pong (mitad del tiempo)
        const uint32_t half = delay_samples_ >> 1;
        const uint32_t rpos_b = (write_pos_ - (half < DL_MIN_SAMPLES ? DL_MIN_SAMPLES : half)) & DL_BUF_MASK;
        const float tap_b = static_cast<float>(buf_[rpos_b]) * (1.f / 32768.f);

        // Feedback musical: lowpass + saturación + smear.
        // El duck se aplica solo al retorno wet para no colapsar la cola interna.
        float fb = tap_a;
        fb_lp_ += 0.18f * (fb - fb_lp_);
        fb = fb_lp_;
        fb = soft_clip(fb);
        fb = 0.82f * fb + 0.18f * fb_prev_;
        fb_prev_ = fb;

        // Write: mono send + feedback filtrado del tap principal
        const float send = (in_l + in_r) * 0.5f;
        const float w    = soft_clip(send + fb * feedback_);
        buf_[write_pos_] = to_i16(w);
        write_pos_ = (write_pos_ + 1u) & DL_BUF_MASK;

        // Mix paralelo con ducking sobre el retorno wet
        const float wet_now = wet_ * wet_duck_;
        const float out_l = in_l + wet_now * tap_a;
        const float out_r = in_r + wet_now * tap_b;

        inout_l = to_i16(out_l);
        inout_r = to_i16(out_r);
    }

private:
    // delay_ms = (beats * 60000) / bpm
    // beats = kDivBeats12[idx] / 12.0
    void recalc_samples() {
        const float beats   = kDivBeats12[div_index_] * (1.f / 12.f);
        const float time_ms = (beats * 60000.f) / bpm_;
        uint32_t s = static_cast<uint32_t>(time_ms * DL_SAMPLERATE / 1000.f);
        if (s < DL_MIN_SAMPLES)    s = DL_MIN_SAMPLES;
        if (s > DL_BUF_SIZE - 1u) s = DL_BUF_SIZE - 1u;
        delay_samples_ = s;
    }

    static float soft_clip(float x) {
        if (x >  1.0f) return  2.f/3.f;
        if (x < -1.0f) return -2.f/3.f;
        return x - (x * x * x) * (1.f / 3.f);
    }

    static int16_t to_i16(float x) {
        if (x >  0.9999f) return  32767;
        if (x < -0.9999f) return -32768;
        return static_cast<int16_t>(x * 32767.f);
    }

    // 64KB en BSS — no heap
    int16_t  buf_[DL_BUF_SIZE] = {};
    uint32_t write_pos_    = 0;
    uint32_t delay_samples_ = 22050u;
    float    feedback_     = 0.40f;
    float    wet_          = 0.00f;
    float    wet_duck_     = 1.0f;
    bool     active_       = false;
    float    fb_lp_        = 0.0f;
    float    fb_prev_      = 0.0f;
    uint8_t  div_index_    = 4;
    float    bpm_          = 120.0f;
};
