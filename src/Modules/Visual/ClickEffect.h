#pragma once
#include "../Module.h"
#include <vector>

class ClickEffect : public Module {
public:
    ClickEffect();
    void OnRender() override;

private:
    struct Particle {
        float x, y;
        float vx, vy;
        float radius;
        float born;
        float life;
        float r, g, b;
        float baseAlpha;
        bool  filled;
    };

    std::vector<Particle> particles;
    bool lastDown = false;

    void spawnBurst(float mx, float my, float now);
};
