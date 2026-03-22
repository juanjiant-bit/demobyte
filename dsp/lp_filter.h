#pragma once
#include <math.h>

class LpFilter {
public:
    void init(float sample_rate) {
        sample_rate_ = sample_rate;
        cutoff_hz_ = 18000.0f;
        resonance_ = 0.62f;
        z1_l_ = z2_l_ = 0.0f;
        z1_r_ = z2_r_ = 0.0f;
        active_ = false;
        update_coeff();
    }

    void reset() {
        z1_l_ = z2_l_ = 0.0f;
        z1_r_ = z2_r_ = 0.0f;
    }

    void set_cutoff(float hz) {
        if (hz < 200.0f) hz = 200.0f;
        if (hz > 18000.0f) hz = 18000.0f;
        cutoff_hz_ = hz;
        active_ = cutoff_hz_ < 17950.0f;
        update_coeff();
    }

    void set_resonance(float q) {
        if (q < 0.50f) q = 0.50f;
        if (q > 0.80f) q = 0.80f;
        resonance_ = q;
        update_coeff();
    }

    bool is_active() const { return active_; }

    inline void process(float& l, float& r) {
        if (!active_) return;
        l = process_sample(l, z1_l_, z2_l_);
        r = process_sample(r, z1_r_, z2_r_);
    }

private:
    inline float process_sample(float x, float& z1, float& z2) {
        const float y = b0_ * x + z1;
        z1 = b1_ * x - a1_ * y + z2;
        z2 = b2_ * x - a2_ * y;
        return y;
    }

    void update_coeff() {
        const float omega = 2.0f * 3.14159265358979323846f * cutoff_hz_ / sample_rate_;
        const float sn = sinf(omega);
        const float cs = cosf(omega);
        const float alpha = sn / (2.0f * resonance_);

        float b0 = (1.0f - cs) * 0.5f;
        float b1 = 1.0f - cs;
        float b2 = (1.0f - cs) * 0.5f;
        float a0 = 1.0f + alpha;
        float a1 = -2.0f * cs;
        float a2 = 1.0f - alpha;

        if (a0 == 0.0f) a0 = 1.0f;

        b0_ = b0 / a0;
        b1_ = b1 / a0;
        b2_ = b2 / a0;
        a1_ = a1 / a0;
        a2_ = a2 / a0;
    }

    float sample_rate_ = 44100.0f;
    float cutoff_hz_ = 18000.0f;
    float resonance_ = 0.62f;

    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f;
    float a1_ = 0.0f, a2_ = 0.0f;

    float z1_l_ = 0.0f, z2_l_ = 0.0f;
    float z1_r_ = 0.0f, z2_r_ = 0.0f;

    bool active_ = false;
};
