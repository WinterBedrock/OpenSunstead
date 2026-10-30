#pragma once
#include "../Module.h"

// HealthWarning — fades a red vignette around the screen edges when the local
// player's health drops below a threshold. Ported in spirit from Latite's
// HealthWarning (which draws a red vignette on the hud_screen).
class HealthWarning : public Module {
public:
    HealthWarning() : Module("HealthWarning", "Red screen vignette when low on health", Category::Visual, 0) {
        // [0] health threshold (out of 20) below which the vignette appears
        AddSetting(new NumberSetting("Threshold", "Show below this health (HP)", 6.0f, 1.0f, 20.0f, 0.5f));
        // [1] maximum vignette opacity at 0 HP
        AddSetting(new NumberSetting("Opacity", "Maximum vignette opacity", 0.55f, 0.05f, 1.0f, 0.01f));
        // [2] vignette thickness (fraction of screen)
        AddSetting(new NumberSetting("Size", "Vignette thickness (fraction of screen)", 0.28f, 0.05f, 0.6f, 0.01f));
        // [3] color
        AddSetting(new ColorSetting("Color", "Vignette color", 1.0f, 0.0f, 0.0f, 1.0f));
    }

    void OnRender() override;
};
