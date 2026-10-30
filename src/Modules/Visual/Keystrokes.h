#pragma once
#include "../Module.h"
#include <array>

class Keystrokes : public Module {
public:
    Keystrokes() : Module("Keystrokes", "Displays pressed keys on your screen", Category::Visual, 0) {
        mIsHudModule = true;
        AddSetting(new EnumSetting("Style", "Color theme style", 0, {"Theme", "Custom"}));
        AddSetting(new ColorSetting("Text Color", "Color of keystroke characters", 1.0f, 1.0f, 1.0f, 1.0f));
        AddSetting(new NumberSetting("Blur Strength", "Glassmorphism blur intensity", 0.0f, 0.0f, 20.0f, 0.5f));
        AddSetting(new NumberSetting("Background Dim", "Darkness of the background", 0.6f, 0.0f, 1.0f, 0.05f));
        AddSetting(new NumberSetting("Corner Rounding", "Rounding of the key boxes", 4.0f, 0.0f, 15.0f, 0.5f));
    }
    
    void OnRender() override;

private:
    std::array<float, 7> keyAnims = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
};
