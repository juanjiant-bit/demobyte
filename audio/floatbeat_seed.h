#pragma once
#include "float_ops.h"
#include <stdint.h>
struct FloatbeatState {
    float t_f = 0.0f;
};

inline float fb_clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}


inline float floatbeat_parallel_lane(float t, float base_hz, float div_a, float div_b, float offset)
{
    const float ta = t * (base_hz / div_a);
    const float tb = (t + offset) * (base_hz / div_b);
    float x =
        f_sine(ta) * 0.42f +
        f_tri(tb) * 0.26f +
        f_sine((ta * 0.5f) + (tb * 0.125f)) * 0.18f;
    return f_softclip(x);
}

inline float floatbeat_basic(FloatbeatState& st, float dt, float base_hz, float body)
{
    st.t_f += dt;
    if (st.t_f > 4096.0f) st.t_f -= 4096.0f;
    base_hz = fb_clampf(base_hz, 20.0f, 4000.0f);
    body = fb_clampf(body, 0.0f, 1.0f);
    const float t1 = st.t_f * base_hz;
    const float t2 = st.t_f * (base_hz * 0.5f);
    const float t3 = st.t_f * (base_hz * 1.498f);

    const float x =
        f_sine(t1) * (0.55f + 0.20f * body) +
        f_sine(t2) * (0.22f + 0.28f * body) +
        f_sine(t3) * 0.12f;

    return f_softclip(x);
}

inline float floatbeat_algo(FloatbeatState& st, float dt, float base_hz, float body, uint8_t algo)
{
    st.t_f += dt;
    if (st.t_f > 4096.0f) st.t_f -= 4096.0f;
    body = fb_clampf(body, 0.0f, 1.0f);
    const float t = st.t_f;
    const float f = fb_clampf(base_hz, 20.0f, 4000.0f);

    switch (algo & 3u) {
        default:
        case 0u: {
            // deep sine stack / sub body (more stable, more low-end)
            const float x =
                f_sine(t * f) * (0.54f + 0.18f * body) +
                f_sine(t * (f * 0.5f)) * (0.18f + 0.30f * body) +
                f_sine(t * (f * 1.5f)) * 0.08f + f_sine(t * (f * 0.25f)) * 0.10f;
            return f_softclip(x);
        }
        case 1u: {
            // metallic / bright hybrid (stronger edge in dry mode)
            const float x =
                f_sine(t * (f * 1.0f)) * (0.40f + 0.12f * body) +
                f_tri (t * (f * 2.01f)) * 0.30f +
                f_sine(t * (f * 3.97f)) * 0.11f;
            return f_softclip(x * 0.90f);
        }
        case 2u: {
            // sub bass focused (heavier sub emphasis)
            const float x =
                f_sine(t * (f * 0.5f)) * (0.56f + 0.34f * body) +
                f_sine(t * (f * 1.0f)) * (0.28f + 0.16f * body) +
                f_tri (t * (f * 0.25f)) * 0.10f + f_sine(t * (f * 0.125f)) * 0.06f;
            return f_softclip(x);
        }
        case 3u: {
            // noisy resonant-ish texture (more unstable/expressive in dry mode)
            const float wob = f_sine(t * 0.73f) * 0.11f;
            const float x =
                f_tri (t * (f * (1.0f + wob))) * 0.32f +
                f_sine(t * (f * 1.33f)) * (0.34f + 0.10f * body) +
                f_sine(t * (f * 0.5f)) * 0.18f;
            return f_softclip(x * 0.92f);
        }
    }
}
