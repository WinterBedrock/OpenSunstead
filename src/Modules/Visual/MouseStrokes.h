#pragma once
#include "../Module.h"
#include "../../SDK/Types.h"

class MouseStrokes : public Module {
public:
    MouseStrokes() : Module("MouseStrokes", "Visualises your camera/mouse movement", Category::Visual, 0) {
        mIsHudModule = true;
        AddSetting(new NumberSetting("Scale", "Overall panel size", 1.0f, 0.3f, 3.0f, 0.05f));
        AddSetting(new NumberSetting("Rounding", "Corner radius of the panel", 10.0f, 0.0f, 30.0f, 0.5f));
        AddSetting(new BoolSetting("Glow", "Draw an outer glow around the panel", false));
        AddSetting(new BoolSetting("Border", "Draw a border outline", false));
        AddSetting(new BoolSetting("Shadow", "Draw a drop shadow", true));
        AddSetting(new ColorSetting("Background", "Panel background colour", 0.0f, 0.0f, 0.0f, 0.55f));
        AddSetting(new ColorSetting("Cursor", "Cursor circle colour", 1.0f, 1.0f, 1.0f, 1.0f));
        AddSetting(new ColorSetting("Glow Color", "Outer glow colour", 0.3f, 0.6f, 1.0f, 0.5f));
        AddSetting(new NumberSetting("Cursor Size", "Size of the cursor circle", 0.08f, 0.02f, 0.25f, 0.01f));
        AddSetting(new NumberSetting("Border Thickness", "Thickness of the border outline", 1.5f, 0.5f, 5.0f, 0.1f));
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnRender() override;

    struct CircleTrail {
        float x, y, opacity;
    };

    std::vector<CircleTrail> trails;

    float mDeltaX = 0.0f;
    float mDeltaY = 0.0f;
    float mCursorX = 0.0f;
    float mCursorY = 0.0f;
    Vec2 mPrevRot;
    bool mHasPrev = false;
};
