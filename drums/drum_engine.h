#pragma once

namespace drums {

class DrumEngine {
public:
    void init();
    void trigger_kick();
    void trigger_snare();
    void trigger_hat();
    float kick_env() const;
    float render(float color);

private:
    float kick_env_ = 0.0f;
    float snare_env_ = 0.0f;
    float hat_env_ = 0.0f;
    float kick_phase_ = 0.0f;
    float snare_tone_phase_ = 0.0f;
    unsigned noise_ = 0x12345678u;
};

}  // namespace drums
