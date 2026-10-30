#pragma once
#include "../Module.h"
#include <deque>

// FpsGraph — recent FPS sparkline as a HUD element (HudEditor-draggable).
class FpsGraph : public Module {
public:
    FpsGraph() : Module("FpsGraph", "Recent FPS graph overlay", Category::Visual, 0) {
        mIsHudModule = true;
        mPosX = 10.0f;
        mPosY = 90.0f;
        mWidth = 160.0f;
        mHeight = 56.0f;
        AddSetting(new NumberSetting("History", "Seconds of FPS history", 4.0f, 1.0f, 15.0f, 0.5f));     // [0]
        AddSetting(new NumberSetting("Background Blur", "Blur intensity", 0.0f, 0.0f, 20.0f, 0.5f));    // [1]
        AddSetting(new NumberSetting("Background Dim", "Panel opacity", 0.78f, 0.0f, 1.0f, 0.05f));      // [2]
        AddSetting(new NumberSetting("Background Rounding", "Corner rounding", 5.0f, 0.0f, 20.0f, 1.0f)); // [3]
        AddSetting(new BoolSetting("Show Value", "Draw current FPS number", true));                      // [4]
    }

    void OnRender() override;

private:
    float GetHistory() const { return static_cast<NumberSetting*>(mSettings[0])->mValue; }
    float GetBlur() const { return static_cast<NumberSetting*>(mSettings[1])->mValue; }
    float GetDim() const { return static_cast<NumberSetting*>(mSettings[2])->mValue; }
    float GetRounding() const { return static_cast<NumberSetting*>(mSettings[3])->mValue; }
    bool  GetShowValue() const { return static_cast<BoolSetting*>(mSettings[4])->mValue; }

    struct Sample { float t; float fps; };
    std::deque<Sample> mSamples;
    float mSmoothFps = 0.0f;
};
