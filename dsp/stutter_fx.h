#pragma once
// stutter_fx.h — BYT3 V1.18
//
// Beat Repeat sincronizado al BPM.
//
// ARQUITECTURA:
//   Captura continua en ring buffer mono (750ms máximo).
//   Al activarse: congela un fragmento de longitud = división del BPM.
//   El fragmento se repite en loop exacto con crossfade dry/wet.
//
// DISEÑO DEL READ POINTER:
//   Se trackea el PROGRESO dentro del loop en Q16 (0..loop_len_<<16),
//   no una posición absoluta en el buffer. Esto evita todos los problemas
//   de wrap cuando el fragmento cruza el boundary del ring buffer.
//   Índice real = (loop_start_ + progress >> 16) % BUF_SAMPLES
//
// BUFFER: 33075 samples = 750ms @ 44100Hz (mono int16, ~64KB BSS)
//
// STEREO OUTPUT:
//   Canal L: tap en progress_in_loop.
//   Canal R: tap en (progress_in_loop + stereo_offset_) % loop_len_.
//   Genera pseudo-estéreo sin buffer adicional.
//
#include <cstdint>
#include <cstring>

class StutterFx {
public:
    static constexpr uint32_t BUF_SAMPLES = 33075u;  // 750ms @ 44100Hz

    void init() {
        memset(buf_, 0, sizeof(buf_));
        write_pos_        = 0;
        progress_q16_     = 0;
        loop_start_       = 0;
        loop_len_         = BUF_SAMPLES;
        rate_q16_         = (1u << 16);
        depth_target_q15_ = 0;
        depth_smooth_q15_ = 0;
        stereo_offset_    = BUF_SAMPLES / 2;
        gate_on_          = false;
    }

    // ── Gate control ──────────────────────────────────────────────────────
    // div_samples: longitud del fragmento en samples (calculado desde BPM).
    void gate_on(uint32_t div_samples) {
        if (div_samples < 64u)         div_samples = 64u;
        if (div_samples > BUF_SAMPLES) div_samples = BUF_SAMPLES;
        gate_on_      = true;
        loop_len_     = div_samples;
        // Capturar el fragmento más reciente
        loop_start_   = (write_pos_ + BUF_SAMPLES - loop_len_) % BUF_SAMPLES;
        progress_q16_ = 0;  // empezar desde el inicio del fragmento
        // Offset R: la mitad del fragmento (mínimo 10ms)
        stereo_offset_ = (loop_len_ >> 1);
        if (stereo_offset_ < 441u) stereo_offset_ = 441u;
        if (stereo_offset_ >= loop_len_) stereo_offset_ = loop_len_ - 1u;
        depth_target_q15_ = 32767;
    }

    void gate_off() {
        gate_on_          = false;
        depth_target_q15_ = 0;
    }

    bool is_active() const { return gate_on_ || depth_smooth_q15_ > 32; }

    // Mantener API de rate (no usado en beat repeat normal)
    void set_rate(float r) {
        if (r < 0.25f) r = 0.25f;
        if (r > 4.0f)  r = 4.0f;
        rate_q16_ = (uint32_t)(r * 65536.0f);
    }

    // ── Procesamiento ─────────────────────────────────────────────────────
    void process(int16_t& in_l, int16_t& in_r) {
        // Captura continua (siempre, independiente del gate)
        const int16_t mono_in = (int16_t)(((int32_t)in_l + in_r) >> 1);
        buf_[write_pos_] = mono_in;
        write_pos_ = (write_pos_ + 1u) % BUF_SAMPLES;

        // Suavizar depth
        depth_smooth_q15_ += (int32_t)(
            ((int64_t)(depth_target_q15_ - depth_smooth_q15_) * DEPTH_ALPHA_Q15) >> 15
        );

        if (!gate_on_ && depth_smooth_q15_ < 32) {
            depth_smooth_q15_ = 0;
            return;
        }

        // ── Calcular índices de lectura usando progreso dentro del loop ────
        // progress_q16_ es 0..loop_len_*65536 — sin dependencia del wrap del buffer
        const uint32_t prog_samples = progress_q16_ >> 16;
        const int32_t  frac_q15     = (int32_t)((progress_q16_ & 0xFFFFu) >> 1);
        const int32_t  inv_q15      = 32767 - frac_q15;

        // Canal L: leer desde loop_start_ + prog_samples (con wrap del buffer)
        const uint32_t idx_l  = (loop_start_ + prog_samples) % BUF_SAMPLES;
        const uint32_t idx_l1 = (idx_l + 1u) % BUF_SAMPLES;
        const int32_t loop_l  = ((int32_t)buf_[idx_l] * inv_q15 +
                                  (int32_t)buf_[idx_l1] * frac_q15) >> 15;

        // Canal R: offset dentro del loop (siempre < loop_len_, nunca underflow)
        const uint32_t prog_r = (prog_samples + stereo_offset_) % loop_len_;
        const uint32_t idx_r  = (loop_start_ + prog_r) % BUF_SAMPLES;
        const uint32_t idx_r1 = (idx_r + 1u) % BUF_SAMPLES;
        const int32_t loop_r  = ((int32_t)buf_[idx_r] * inv_q15 +
                                  (int32_t)buf_[idx_r1] * frac_q15) >> 15;

        // ── Avanzar progreso y resetear al fin del fragmento ──────────────
        progress_q16_ += rate_q16_;
        if (progress_q16_ >= (loop_len_ << 16)) {
            progress_q16_ = 0;
        }

        // ── Crossfade dry/wet ─────────────────────────────────────────────
        const int32_t wet = depth_smooth_q15_;
        const int32_t dry = 32767 - wet;
        int32_t out_l = ((int32_t)in_l * dry + loop_l * wet) >> 15;
        int32_t out_r = ((int32_t)in_r * dry + loop_r * wet) >> 15;
        if (out_l >  32767) out_l =  32767;
        if (out_l < -32768) out_l = -32768;
        if (out_r >  32767) out_r =  32767;
        if (out_r < -32768) out_r = -32768;
        in_l = (int16_t)out_l;
        in_r = (int16_t)out_r;
    }

private:
    static constexpr int32_t DEPTH_ALPHA_Q15 = 1638;  // tau ~30ms @ 44100Hz

    int16_t  buf_[BUF_SAMPLES] = {};
    uint32_t write_pos_        = 0;
    uint32_t progress_q16_     = 0;   // progreso dentro del loop, 0..loop_len_<<16
    uint32_t loop_start_       = 0;   // inicio del fragmento en el buffer circular
    uint32_t loop_len_         = BUF_SAMPLES;
    uint32_t rate_q16_         = (1u << 16);
    uint32_t stereo_offset_    = BUF_SAMPLES / 2;
    int32_t  depth_target_q15_ = 0;
    int32_t  depth_smooth_q15_ = 0;
    bool     gate_on_          = false;
};
