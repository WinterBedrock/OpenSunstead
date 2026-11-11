#pragma once
#include "../Module.h"

class HealthWarning : public Module {
public:
    HealthWarning() : Module("HealthWarning", "Red screen vignette when low on health", Category::Visual, 0) {
        AddSetting(new NumberSetting("Threshold", "Show below this health (HP)", 6.0f, 1.0f, 20.0f, 0.5f));
        AddSetting(new NumberSetting("Opacity", "Maximum vignette opacity", 0.55f, 0.05f, 1.0f, 0.01f));
        AddSetting(new NumberSetting("Size", "Vignette thickness (fraction of screen)", 0.28f, 0.05f, 0.6f, 0.01f));
        AddSetting(new ColorSetting("Color", "Vignette color", 1.0f, 0.0f, 0.0f, 1.0f));
    }

    void OnRender() override;
};
