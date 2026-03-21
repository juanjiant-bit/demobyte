#pragma once
#include "../dsp/fast_trig.h"
#include <math.h>

inline float f_wrap01(float x) {
    x -= (int)x;
    if (x < 0.0f) x += 1.0f;
    return x;
}

inline float f_sine(float p) {
    return fast_sine01(f_wrap01(p));
}

inline float f_tri(float p) {
    const float x = f_wrap01(p);
    return 4.0f * fabsf(x - 0.5f) - 1.0f;
}

inline float f_softclip(float x) {
    return x / (1.0f + fabsf(x));
}
