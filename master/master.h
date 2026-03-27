#pragma once

namespace master {

class Master {
public:
    void init();
    void set_volume(float x);
    float process(float x);

private:
    float dc_x1_ = 0.0f;
    float hp_y1_ = 0.0f;
    float env_ = 0.0f;
    float volume_ = 1.0f;
};

}  // namespace master
