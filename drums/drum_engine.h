
#pragma once

namespace drums {

class DrumEngine {
public:
    void init();

    void set_decay(float x);
    void set_tone(float x);

    void trigger_kick();
    void trigger_snare();
    void trigger_hat();
    void trigger_perc();

    float render();

private:
    float decay_ = 0.5f;
    float tone_ = 0.5f;

    float kick_env_ = 0.0f;
    float kick_phase_ = 0.0f;

    float snare_env_ = 0.0f;
    float snare_phase_ = 0.0f;

    float hat_env_ = 0.0f;
    float hat_hp_z_ = 0.0f;

    float perc_env_ = 0.0f;
    float perc_phase_ = 0.0f;

    unsigned noise_ = 0x12345678u;
};

}  // namespace drums
