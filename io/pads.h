#pragma once
#include "pico/stdlib.h"
#include <stdint.h>

namespace pads {

static constexpr int kNumPads = 4;
static constexpr uint kSensePins[kNumPads] = {8, 9, 13, 14};
static constexpr uint kPotPin = 26;

struct PadState {
    uint16_t raw = 0;
    uint16_t baseline = 0;
    bool pressed = false;
    bool trigger = false;
    bool release = false;
    float pressure = 0.0f;
    uint8_t on_count = 0;
    uint8_t off_count = 0;
    uint16_t cooldown_ms = 0;
};

void init();
void update_1ms();

const PadState& get(int idx);
float macro();

}  // namespace pads
