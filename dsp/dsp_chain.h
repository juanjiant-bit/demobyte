#pragma once
// dsp_chain.h — Bytebeat Machine V1.21
// Cadena DSP completa — orden de procesamiento:
//
//   bytebeat+drums mix
//   ↓
//   Stutter FX        (buffer 20ms)
//   ↓
//   DC Blocker        (HPF ~35Hz)
//   ↓
//   HP Filter         (SHIFT+POT5 / MIDI CC74 → 20Hz–8kHz momentáneo)   [nuevo V1.14]
//   ↓
//   Snap Gate         (base layer POT5 → todo se vuelve tick)     [nuevo V1.14]
//   ↓
//   Soft Clip         (drive 0–1)
//   ↓
//   Chorus            (SHIFT+REC+POT2 / MIDI CC93 → BBD stereo)              [nuevo V1.14]
//   ↓
//   Grain Freeze      (SHIFT+POT4 → loop congelado)          [nuevo V1.14]
//   ↓
//   Reverb            (Schroeder/Freeverb)
//   ↓
//   Limiter           (threshold=0.9 FS, release ~30ms)
//   ↓
//   PWM output
//
#include <cstdint>
#include "stutter_fx.h"
#include "reverb.h"
#include "chorus.h"
#include "hp_filter.h"
#include "lp_filter.h"
#include "grain_freeze.h"
#include "snap_gate.h"
#include "delay_line.h"
#include "exciter.h"
#include "../audio/dsp/dc_blocker.h"
#include "../audio/dsp/soft_clip.h"
#include "../audio/dsp/limiter.h"

struct LevelScaler {
    static int16_t scale(int16_t x, int32_t gain_q8) {
        int32_t v = ((int32_t)x * gain_q8) >> 8;
        if (v >  32767) return  32767;
        if (v < -32768) return -32768;
        return (int16_t)v;
    }
    static constexpr int32_t SYNTH_GAIN = 128;
    static constexpr int32_t DRUM_GAIN  = 128;
    static constexpr int32_t KICK_GAIN  = 128;
    static constexpr int32_t SNARE_GAIN =  91;
    static constexpr int32_t HAT_GAIN   =  91;
};

class DspChain {
public:
    void init();
    void reset();

    // Parámetros capa 1 (SHIFT)
    void set_drive(float d);

    void warm_dc(int16_t first_sample) {
        dc_l_.warm(first_sample);
        dc_r_.warm(first_sample);
    }

    // Parámetros capa 2 (SHIFT+REC) — V1.14
    void set_chorus_amount(float a);
    void set_hp_amount(float a);
    void set_grain_amount(float a);
    void set_snap_amount(float a);
    void set_snap_bpm(float b);
    void set_exciter_amount(float a);
    void set_dual_filter_control(float norm);
    void set_delay_wet_duck(float d) { delay_.set_wet_duck(d); }
    inline void set_delay_wet_duck_fast(float d) { delay_.set_wet_duck_fast(d); }

    // V1.17: Delay tempo-sync
    void set_delay_div(float d);
    void set_delay_fb(float fb);
    void set_delay_wet(float w);
    void set_delay_bpm(float b);

    // Acceso a submódulos
    StutterFx&   stutter()    { return stutter_; }
    Reverb&      reverb()     { return reverb_;  }
    Chorus&      chorus()     { return chorus_;  }
    HpFilter&    hp_filter()  { return hpf_;     }
    GrainFreeze& grain()      { return grain_;   }
    SnapGate&    snap_gate()  { return snap_;    }
    DelayLine&   delay()      { return delay_;   }
    Exciter&     exciter()    { return exciter_; }

    void process(int16_t& left, int16_t& right);

    int32_t limiter_gain_l() const { return lim_lr_.gain_q15_; }


private:
    enum FxMask : uint32_t {
        FX_STUTTER = 1u << 0,
        FX_HP      = 1u << 1,
        FX_LP      = 1u << 2,
        FX_SNAP    = 1u << 3,
        FX_EXCITER = 1u << 4,
        FX_CHORUS  = 1u << 5,
        FX_GRAIN   = 1u << 6,
        FX_REVERB  = 1u << 7,
        FX_DELAY   = 1u << 8,
    };

    static bool changed(float a, float b, float eps = 0.0005f);
    void refresh_active_mask();

    float drive_cache_ = -1.0f;
    float chorus_cache_ = -1.0f;
    float hp_cache_ = -1.0f;
    float grain_cache_ = -1.0f;
    float snap_cache_ = -1.0f;
    float snap_bpm_cache_ = -1.0f;
    float exciter_cache_ = -1.0f;
    float delay_div_cache_ = -1.0f;
    float delay_fb_cache_ = -1.0f;
    float delay_wet_cache_ = -1.0f;
    float delay_bpm_cache_ = -1.0f;
    float hp_mix_ = 0.0f;
    float lp_mix_ = 0.0f;
    uint32_t active_mask_ = 0;

    DcBlocker   dc_l_,   dc_r_;
    SoftClip    clip_l_, clip_r_;
    Limiter     lim_lr_;
    StutterFx   stutter_;
    Reverb      reverb_;
    // V1.14: nuevos efectos de la capa SHIFT+REC
    Chorus      chorus_;
    HpFilter    hpf_;
    LpFilter    lp_;
    GrainFreeze grain_;
    SnapGate    snap_;
    // Stage1A
    Exciter     exciter_;
    // V1.17
    DelayLine   delay_;   // 64KB BSS
};
