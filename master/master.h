#pragma once

namespace master {

class Master {
public:
    void init();
    float process(float x);

private:
    float dc_z_ = 0.0f;
    float hp_z_ = 0.0f;
    float env_ = 0.0f;
};

}  // namespace master
