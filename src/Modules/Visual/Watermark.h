#pragma once
#include "../Module.h"

class Watermark : public Module {
public:
    Watermark() : Module("Watermark", "Displays the client watermark on screen", Category::Visual, 0) {
        mIsHudModule = true;
        mScale = 2.4f;
        mPosX = 18.0f;
        mPosY = 14.0f;
        AddSetting(new BoolSetting("Glow", "Draw glow behind the watermark", true));
        AddSetting(new NumberSetting("Glow Strength", "Glow alpha strength multiplier", 0.5f, 0.1f, 1.0f, 0.05f));
        AddSetting(new BoolSetting("Show Info", "Display extra information like FPS", false));
        AddSetting(new NumberSetting("Text Shadow", "Drop-shadow strength behind the text (0 = off)", 0.0f, 0.0f, 1.0f, 0.05f));
    }

    void OnRender() override;
};
