#pragma once

namespace drums {

class DrumEngine {
public:
    void init();
    void trigger_kick();
    void trigger_snare();
    void trigger_hat();
    float render(float color, float pressure2, float pressure3, float pressure4);
    float kick_env() const { return kick_env_; }

private:
    unsigned int noise_ = 0x87654321u;
    float kick_env_ = 0.0f;
    float kick_phase_ = 0.0f;
    float snare_env_ = 0.0f;
    float hat_env_ = 0.0f;
};

} // namespace drums
