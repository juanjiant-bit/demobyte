#pragma once
#include "audio_output.h"
#include "hardware/pio.h"

class AudioOutputI2S : public AudioOutput {
public:
    static constexpr uint8_t  PIN_BCLK    = 10;
    static constexpr uint8_t  PIN_LRCK    = 11;   // pin_bclk + 1
    static constexpr uint8_t  PIN_DIN     = 12;
    static constexpr uint32_t SAMPLE_RATE = 44100;

    void init()  override;
    void write(int16_t left, int16_t right) override;
    void start() override {}
    void stop()  override {}

private:
    PIO  pio_         = pio0;
    uint sm_          = 0;
    uint offset_      = 0;
    bool initialized_ = false;
};
