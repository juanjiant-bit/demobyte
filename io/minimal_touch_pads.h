#pragma once

#include <cstdint>

// Minimal 1x4 capacitive pad reader for the already-built prototype.
// Wiring confirmed from the latest sketch/photo:
//   DRIVE/ROW0 -> GP5
//   PAD1       -> GP8   (bytebeat drone/random / SNAP)
//   PAD2       -> GP9   (kick)
//   PAD3       -> GP13  (snare)
//   PAD4       -> GP14  (hat)
// Same measurement idea as the matrix handler: charge time from one drive pin
// into each sense pad independently.
class MinimalTouchPads {
public:
    static constexpr uint8_t NUM_PADS = 4;
    static constexpr uint8_t DRIVE_PIN = 5;
    static constexpr uint8_t PAD_PINS[NUM_PADS] = {8, 9, 13, 14};

    struct Preset {
        uint32_t discharge_us;
        uint32_t max_charge_us;
        uint32_t hyst_on_us;
        uint32_t hyst_off_us;
        uint8_t calib_samples;
        float baseline_alpha;

        static constexpr Preset PROTOTYPE() {
            return {220, 2600, 20, 10, 220, 0.0018f};
        }
    };

    void init(Preset p = Preset::PROTOTYPE());
    void calibrate();
    void scan();

    bool is_pressed(uint8_t pad) const;
    bool just_pressed(uint8_t pad) const;
    bool just_released(uint8_t pad) const;
    uint16_t get_state() const { return state_confirmed_; }
    uint32_t get_raw_us(uint8_t pad) const;
    uint32_t get_baseline_us(uint8_t pad) const;

private:
    uint32_t measure_charge_us(uint8_t pin);

    Preset preset_{};
    float baseline_f_[NUM_PADS] = {};
    uint32_t raw_us_[NUM_PADS] = {};
    uint16_t state_confirmed_ = 0;
    uint16_t state_prev_ = 0;
};
