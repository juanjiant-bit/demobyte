#pragma once
#include <cstdint>

namespace audio {

class AudioOutputI2S {
public:
    void init();
    void write(int16_t left, int16_t right);

private:
    static inline uint32_t slot_from_s16(int16_t sample) {
        return static_cast<uint32_t>(static_cast<uint16_t>(sample)) << 16;
    }
};

} // namespace audio
