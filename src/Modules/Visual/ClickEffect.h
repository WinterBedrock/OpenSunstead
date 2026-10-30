#pragma once
#include "../Module.h"
#include <vector>

// ClickEffect: spawns small animated circles at the cursor position on every
// mouse click. Renders on all ScreenViews EXCEPT the in-game HUD (hud_screen),
// so it shows up in menus / inventory / clickgui etc. Fully configurable
// (count, size, lifetime, colors, opacity randomness).
class ClickEffect : public Module {
public:
    ClickEffect();
    void OnRender() override;

private:
    struct Particle {
        float x, y;          // screen position
        float vx, vy;        // velocity (px/sec)
        float radius;        // base radius
        float born;          // spawn time (seconds)
        float life;          // lifetime (seconds)
        float r, g, b;       // color 0..1
        float baseAlpha;     // starting alpha
        bool  filled;        // filled or ring
    };

    std::vector<Particle> particles;
    bool lastDown = false;

    void spawnBurst(float mx, float my, float now);
};
