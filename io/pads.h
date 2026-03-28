#pragma once
#include "pico/stdlib.h"
#include <stdint.h>

namespace controls {

static constexpr int kNumPads = 4;
static constexpr int kNumPots = 3;
static constexpr uint kPadPins[kNumPads] = {8, 9, 13, 14};
static constexpr uint kPotPins[kNumPots] = {26, 27, 28};

struct PadState {
    uint16_t raw = 0;

    bool pressed = false;
    bool trigger = false;
    bool release = false;
    bool held = false;

    float pressure = 0.0f;

    uint32_t touch_start_ms = 0;
    uint32_t last_trigger_ms = 0;
};

struct PotState {
    uint16_t raw = 0;
    float value = 0.0f;
    float stable = 0.0f;
};

void init();
void update_1ms();

const PadState& pad(int idx);
float volume();
float morph();
float color();

}
