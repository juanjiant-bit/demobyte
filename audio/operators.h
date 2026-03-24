#pragma once
#include <stdint.h>

float op_sine(float phase);
float op_tri(float phase);
float op_noise(uint32_t &state);
float op_decay(float &state, float rate);
float op_lp(float x, float &z, float coeff);
float op_fold(float x, float amount);
float op_pm(float phase, float mod, float index);
float op_ring(float a, float b);
float op_slew(float x, float& z, float rate);
