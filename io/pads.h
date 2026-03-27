#pragma once
#include <cstdint>

namespace io {

struct PadState {
    bool pressed = false;
    bool trigger = false;
    bool release = false;
    float pressure = 0.0f;
    uint16_t baseline = 0;
    uint16_t raw = 0;
    uint8_t on_count = 0;
    uint8_t off_count = 0;
    uint8_t cooldown = 0;
};

class Pads {
public:
    void init();
    void update();
    const PadState& get(int index) const { return pads_[index]; }

private:
    PadState pads_[4];
    uint16_t read_pad(int idx);
};

} // namespace io
