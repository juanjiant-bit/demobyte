#pragma once
namespace master {

class Master {
public:
    void init();
    float process(float x, float volume);

private:
    float hp_x1_ = 0.0f;
    float hp_y1_ = 0.0f;
    float env_ = 0.0f;
};

} // namespace master
