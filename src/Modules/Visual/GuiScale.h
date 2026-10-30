#pragma once
#include "../Module.h"

// GuiScale - overrides Minecraft's GUI scale by writing the guiScale /
// guiScaleFrac / guiSize fields on GuiData every tick (mirrors Latite's
// GuiscaleChanger for 1.26.30).
class GuiScale : public Module {
public:
    GuiScale() : Module("GuiScale", "Override the in-game GUI scale", Category::Visual, 0) {
        AddSetting(new NumberSetting("Scale", "Target GUI scale", 3.0f, 0.5f, 5.0f, 0.5f));
    }
    void OnTick() override;
};
