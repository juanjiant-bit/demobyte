#pragma once
#include <cstdint>

class AdcHandler {
public:
    void init();
    void poll();
    float get() const { return smoothed_; }
    uint16_t raw() const { return raw_; }

private:
    static constexpr float SMOOTH = 0.12f;
    uint16_t raw_ = 0;
    float smoothed_ = 0.0f;
};
