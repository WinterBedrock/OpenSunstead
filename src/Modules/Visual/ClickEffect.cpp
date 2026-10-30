#include "ClickEffect.h"
#include "../Setting.h"
#include "../../SDK/Client/ScreenView.h"
#include "../../SDK/Runtime/Guard.h"
#include <imgui.h>
#include <Windows.h>
#include <cstdlib>
#include <cmath>
#include <chrono>

ClickEffect::ClickEffect()
    : Module("ClickEffect", "Spawns animated circles where you click", Category::Visual, 0) {
    AddSetting(new NumberSetting("Count", "Particles spawned per click", 6.0f, 1.0f, 24.0f, 1.0f));
    AddSetting(new NumberSetting("Size", "Base circle radius", 5.0f, 1.0f, 20.0f, 0.5f));
    AddSetting(new NumberSetting("Lifetime", "Seconds before a circle fades out", 0.6f, 0.1f, 3.0f, 0.05f));
    AddSetting(new NumberSetting("Spread", "How far circles drift from the click", 38.0f, 0.0f, 160.0f, 2.0f));
    AddSetting(new BoolSetting("Rainbow", "Randomize circle colors", true));
    AddSetting(new ColorSetting("Color", "Base color when Rainbow is off", 0.45f, 0.75f, 1.0f, 1.0f));
    AddSetting(new BoolSetting("Random Opacity", "Some circles start semi-transparent", true));
}

static float nowSeconds() { return (float)SDK::nowSeconds(); }
static float frand() { return SDK::randUnit(); }

void ClickEffect::spawnBurst(float mx, float my, float now) {
    float count   = static_cast<NumberSetting*>(mSettings[0])->mValue;
    float size    = static_cast<NumberSetting*>(mSettings[1])->mValue;
    float life    = static_cast<NumberSetting*>(mSettings[2])->mValue;
    float spread  = static_cast<NumberSetting*>(mSettings[3])->mValue;
    bool  rainbow = static_cast<BoolSetting*>(mSettings[4])->mValue;
    ColorSetting* col = static_cast<ColorSetting*>(mSettings[5]);
    bool  randA   = static_cast<BoolSetting*>(mSettings[6])->mValue;

    int n = (int)count;
    for (int i = 0; i < n; ++i) {
        Particle p{};
        float ang = frand() * 6.2831853f;
        float spd = spread * (0.35f + frand() * 0.65f);
        p.x = mx; p.y = my;
        p.vx = cosf(ang) * spd;
        p.vy = sinf(ang) * spd;
        p.radius = size * (0.5f + frand());
        p.born = now;
        p.life = life * (0.7f + frand() * 0.6f);
        if (rainbow) {
            // pick a vivid random hue
            float h = frand() * 6.0f; float x = 1.0f - fabsf(fmodf(h, 2.0f) - 1.0f);
            if (h < 1) { p.r = 1; p.g = x; p.b = 0; }
            else if (h < 2) { p.r = x; p.g = 1; p.b = 0; }
            else if (h < 3) { p.r = 0; p.g = 1; p.b = x; }
            else if (h < 4) { p.r = 0; p.g = x; p.b = 1; }
            else if (h < 5) { p.r = x; p.g = 0; p.b = 1; }
            else { p.r = 1; p.g = 0; p.b = x; }
        } else { p.r = col->mValue[0]; p.g = col->mValue[1]; p.b = col->mValue[2]; }
        p.baseAlpha = randA ? (0.35f + frand() * 0.65f) : 1.0f;
        p.filled = frand() > 0.4f;
        particles.push_back(p);
    }
}

void ClickEffect::OnRender() {
    // Render on every ScreenView EXCEPT the in-game HUD.
    if (SDK::isHudScreen()) { particles.clear(); return; }

    float now = nowSeconds();

    // Edge-detect a left click and spawn a burst at the cursor. Only react when
    // OUR process owns the foreground window: an earlier version used
    // GetActiveWindow(), which returns the active window of the *calling
    // thread's* message queue and is therefore almost always NULL on the
    // render/hook thread, so no click ever registered.
    HWND fg = GetForegroundWindow();
    bool down = SDK::isGameWindowFocused() && ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    if (down && !lastDown && fg) {
        POINT pt; if (GetCursorPos(&pt)) {
            ScreenToClient(fg, &pt);
            spawnBurst((float)pt.x, (float)pt.y, now);
        }
    }
    lastDown = down;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (size_t i = 0; i < particles.size();) {
        Particle& p = particles[i];
        float t = (now - p.born) / p.life; // 0..1
        if (t >= 1.0f) { particles.erase(particles.begin() + i); continue; }

        float ease = 1.0f - (1.0f - t) * (1.0f - t); // ease-out
        float px = p.x + p.vx * t;
        float py = p.y + p.vy * t;
        float rad = p.radius * (1.0f + ease * 0.8f);
        float alpha = p.baseAlpha * (1.0f - t);
        ImU32 c = ImGui::ColorConvertFloat4ToU32(ImVec4(p.r, p.g, p.b, alpha));
        if (p.filled) dl->AddCircleFilled(ImVec2(px, py), rad, c, 24);
        else dl->AddCircle(ImVec2(px, py), rad, c, 24, 1.6f);
        ++i;
    }
}