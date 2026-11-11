#pragma once
#include "../Module.h"

class GuiScale : public Module {
public:
    GuiScale() : Module("GuiScale", "Override the in-game GUI scale", Category::Visual, 0) {
        AddSetting(new NumberSetting("Scale", "Target GUI scale", 3.0f, 0.5f, 5.0f, 0.5f));
    }
    void OnTick() override;
};
