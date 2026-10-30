#pragma once
#include "../Module.h"
#include "CounterStyle.h"

// FPS HUD counter — the current framerate on a counter plate ("144 FPS").
class FPS : public Module {
public:
    FPS() : Module("FPS", "Displays the current framerate", Category::Visual, 0) {
        mIsHudModule = true;
        mPosX = 10.0f;
        mPosY = 50.0f;
        AddSetting(new ColorSetting("Text Color", "Color of the suffix text", 1.0f, 1.0f, 1.0f, 1.0f));
        AddSetting(new NumberSetting("Background Blur", "Blur intensity behind the panel", 10.0f, 0.0f, 20.0f, 0.5f));
        AddSetting(new NumberSetting("Background Dim", "Opacity of the background", 0.30f, 0.0f, 1.0f, 0.05f));
        AddSetting(new NumberSetting("Background Rounding", "Roundness of the background corners", 5.0f, 0.0f, 20.0f, 1.0f));
        AddSetting(new BoolSetting("Text Shadow", "Drop shadow behind the text", false));
        AddSetting(new NumberSetting("Shadow Opacity", "Opacity of the plate drop shadow", 0.35f, 0.0f, 1.0f, 0.05f));
        AddSetting(new BoolSetting("Gradient Numbers", "Draw the value in the theme gradient", true));
        AddSetting(new BoolSetting("Number Animation", "Animate the value when it changes", true));
    }

    void OnRender() override;
};
