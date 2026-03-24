// dsp_chain.cpp — Bytebeat Machine V1.21
#include "dsp_chain.h"

// Cachear parámetros acá evita trabajo redundante cuando Core1 y el
// control-rate vuelven a empujar el mismo valor. También compactamos
// las ramas del hot path con un bitmask de efectos activos.

bool DspChain::changed(float a, float b, float eps) {
    return (a < 0.0f) || (a - b > eps) || (b - a > eps);
}

void DspChain::refresh_active_mask() {
    uint32_t mask = 0;
    if (stutter_.is_active()) mask |= FX_STUTTER;
    if (hpf_.is_active())     mask |= FX_HP;
    if (lp_.is_active())      mask |= FX_LP;
    if (snap_.is_active())    mask |= FX_SNAP;
    if (exciter_.is_active()) mask |= FX_EXCITER;
    if (chorus_.is_active())  mask |= FX_CHORUS;
    if (grain_.is_active())   mask |= FX_GRAIN;
    if (reverb_.is_active())  mask |= FX_REVERB;
    if (delay_.is_active())   mask |= FX_DELAY;
    active_mask_ = mask;
}

void DspChain::set_drive(float d) {
    if (!changed(drive_cache_, d)) return;
    clip_l_.set_drive_f(d);
    clip_r_.set_drive_f(d);
    drive_cache_ = d;
}

void DspChain::set_chorus_amount(float a) {
    if (!changed(chorus_cache_, a)) return;
    chorus_.set_amount(a);
    chorus_cache_ = a;
    refresh_active_mask();
}

void DspChain::set_hp_amount(float a) {
    if (!changed(hp_cache_, a)) return;
    hpf_.set_amount(a);
    hp_cache_ = a;
    refresh_active_mask();
}

void DspChain::set_grain_amount(float a) {
    if (!changed(grain_cache_, a)) return;
    grain_.set_amount(a);
    grain_cache_ = a;
    refresh_active_mask();
}

void DspChain::set_snap_amount(float a) {
    if (!changed(snap_cache_, a)) return;
    snap_.set_amount(a);
    snap_cache_ = a;
    refresh_active_mask();
}

void DspChain::set_snap_bpm(float b) {
    if (!changed(snap_bpm_cache_, b, 0.01f)) return;
    snap_.set_bpm(b);
    snap_bpm_cache_ = b;
}

void DspChain::set_exciter_amount(float a) {
    if (!changed(exciter_cache_, a)) return;
    exciter_.set_amount(a);
    exciter_cache_ = a;
    refresh_active_mask();
}


void DspChain::set_dual_filter_control(float norm) {
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;

    // 0..0.45 = LP region, 0.45..0.55 = dry crossover, 0.55..1 = HP region
    if (norm <= 0.45f) {
        const float t = norm / 0.45f;
        const float cutoff = 250.0f + t * (18000.0f - 250.0f);
        lp_.set_cutoff(cutoff);
        hpf_.set_amount(0.0f);
        lp_mix_ = 1.0f;
        hp_mix_ = 0.0f;
    } else if (norm < 0.55f) {
        const float t = (norm - 0.45f) / 0.10f;
        lp_.set_cutoff(18000.0f);
        hpf_.set_amount(0.0f);
        lp_mix_ = 1.0f - t;
        hp_mix_ = 0.0f;
    } else {
        const float t = (norm - 0.55f) / 0.45f;
        lp_.set_cutoff(18000.0f);
        hpf_.set_amount(t);
        lp_mix_ = 0.0f;
        hp_mix_ = t;
    }
    refresh_active_mask();
}

void DspChain::set_delay_div(float d) {
    if (!changed(delay_div_cache_, d)) return;
    delay_.set_div(d);
    delay_div_cache_ = d;
}

void DspChain::set_delay_fb(float fb) {
    if (!changed(delay_fb_cache_, fb)) return;
    delay_.set_feedback(fb);
    delay_fb_cache_ = fb;
}

void DspChain::set_delay_wet(float w) {
    if (!changed(delay_wet_cache_, w)) return;
    delay_.set_wet(w);
    delay_wet_cache_ = w;
    refresh_active_mask();
}

void DspChain::set_delay_bpm(float b) {
    if (!changed(delay_bpm_cache_, b, 0.01f)) return;
    delay_.set_bpm(b);
    delay_bpm_cache_ = b;
}


void DspChain::init() {
    reset();
    reverb_.init();
    stutter_.init();
    chorus_.init();
    hpf_.init();
    lp_.init(44100.0f);
    grain_.init();
    snap_.init();
    exciter_.init();
    delay_.clear();
}

void DspChain::reset() {
    dc_l_.reset();   dc_r_.reset();
    lim_lr_.reset();
    hpf_.reset();
    lp_.reset();
    hp_mix_ = 0.0f;
    lp_mix_ = 0.0f;
    active_mask_ = 0;
    drive_cache_ = chorus_cache_ = hp_cache_ = grain_cache_ = snap_cache_ = -1.0f;
    snap_bpm_cache_ = exciter_cache_ = delay_div_cache_ = delay_fb_cache_ = delay_wet_cache_ = delay_bpm_cache_ = -1.0f;
}

void DspChain::process(int16_t& left, int16_t& right) {
    // 1. Stutter (sobre mix crudo, antes de DC block)
    if (active_mask_ & FX_STUTTER) stutter_.process(left, right);

    // 2. DC block
    left  = dc_l_.process(left);
    right = dc_r_.process(right);

    // 3. Dual filter region (LP / dry crossover / HP)
    const uint32_t mask = active_mask_;
    const int16_t dry_l = left;
    const int16_t dry_r = right;

    if ((mask & FX_LP) && lp_mix_ > 0.0f) {
        float fl = (float)dry_l;
        float fr = (float)dry_r;
        lp_.process(fl, fr);
        left = (int16_t)(dry_l + (fl - dry_l) * lp_mix_);
        right = (int16_t)(dry_r + (fr - dry_r) * lp_mix_);
    }

    if ((mask & FX_HP) && hp_mix_ > 0.0f) {
        int16_t hl = dry_l;
        int16_t hr = dry_r;
        hpf_.process(hl, hr);
        left = (int16_t)(left + (hl - left) * hp_mix_);
        right = (int16_t)(right + (hr - right) * hp_mix_);
    }

    // 4. Snap Gate (base layer POT5) — antes del clip para que el gate
    //    actúe sobre la señal completa y no sobre el saturado
    if (mask & FX_SNAP) snap_.process(left, right);

    // 5. Soft clip
    left  = clip_l_.process(left);
    right = clip_r_.process(right);

    // 6. Exciter — agrega presencia sin tocar el cuerpo
    if (mask & FX_EXCITER) exciter_.process(left, right);

    // 7. Chorus (SHIFT+REC+POT2 / MIDI CC93) — después del clip/exciter, sobre señal limpia
    if (mask & FX_CHORUS) chorus_.process(left, right);

    // 8. Grain Freeze (SHIFT+POT4) — después del chorus
    if (mask & FX_GRAIN) grain_.process(left, right);

    // 9. Reverb
    if (mask & FX_REVERB) {
        int16_t mono_in = (int16_t)(((int32_t)left + right) >> 1);
        reverb_.process(mono_in, left, right);
    }

    // 10. Delay tempo-sync send/return (V1.17)
    if (mask & FX_DELAY) delay_.process(left, right);

    // 11. Limiter stereo-linked
    lim_lr_.process_stereo(left, right);
}
