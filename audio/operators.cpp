#include "operators.h"
#include <math.h>

// fast sine approx (small polynomial)
float op_sine(float phase) {
    float x = phase * 6.2831853f;
    // wrap
    while (x > 3.1415926f) x -= 6.2831853f;
    while (x < -3.1415926f) x += 6.2831853f;
    // fast approx
    const float B = 4.0f / 3.1415926f;
    const float C = -4.0f / (3.1415926f * 3.1415926f);
    float y = B * x + C * x * fabsf(x);
    // improve
    const float P = 0.225f;
    y = P * (y * fabsf(y) - y) + y;
    return y;
}

float op_tri(float phase) {
    float x = phase - (int)phase;
    if (x < 0.0f) x += 1.0f;
    return 4.0f * fabsf(x - 0.5f) - 1.0f;
}

float op_noise(uint32_t &state) {
    state = state * 1664525u + 1013904223u;
    return ((state >> 9) & 0x7FFFFFu) * (1.0f / 4194304.0f) - 1.0f;
}

float op_decay(float &state, float rate) {
    state *= (1.0f - rate);
    return state;
}

float op_lp(float x, float &z, float coeff) {
    z += coeff * (x - z);
    return z;
}

float op_fold(float x, float amount) {
    float k = 1.0f + amount;
    return (x * k) / (1.0f + fabsf(x * k));
}

float op_pm(float phase, float mod, float index) {
    float p = phase + mod * index;
    p -= (int)p;
    if (p < 0.0f) p += 1.0f;
    return op_sine(p);
}

float op_ring(float a, float b) {
    return a * b;
}

float op_slew(float x, float& z, float rate) {
    z += rate * (x - z);
    return z;
}
