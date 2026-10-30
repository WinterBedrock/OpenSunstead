#pragma once
#include "../Module.h"
#include "../../SDK/Types.h"

// MouseKeystrokes -- visualises camera/mouse movement as a small panel with a
// circle cursor that drifts in the direction the player looks, leaving a
// fading trail behind it. Ported from FlarialSRC's Mousestrokes module.
//
// The cursor position is driven by the *delta* of the local player's
// ActorRotationComponent (yaw/pitch) each frame. The accumulated delta
// decays back toward centre when the mouse stops, producing a smooth
// "mouse stroke" visualisation. Settings control scale, rounding, glow,
// border, shadow, cursor size, border thickness, and all colours.
class MouseKeystrokes : public Module {
public:
    MouseKeystrokes() : Module("MouseKeystrokes", "Visualises your camera/mouse movement", Category::Visual, 0) {
        mIsHudModule = true;
        // [0] UI scale multiplier
        AddSetting(new NumberSetting("Scale", "Overall panel size", 1.0f, 0.3f, 3.0f, 0.05f));
        // [1] corner rounding of the panel
        AddSetting(new NumberSetting("Rounding", "Corner radius of the panel", 10.0f, 0.0f, 30.0f, 0.5f));
        // [2] glow / outer shadow colour on the panel
        AddSetting(new BoolSetting("Glow", "Draw an outer glow around the panel", false));
        // [3] border outline on the panel
        AddSetting(new BoolSetting("Border", "Draw a border outline", false));
        // [4] drop shadow beneath the panel
        AddSetting(new BoolSetting("Shadow", "Draw a drop shadow", true));
        // [5] panel background colour
        AddSetting(new ColorSetting("Background", "Panel background colour", 0.0f, 0.0f, 0.0f, 0.55f));
        // [6] cursor circle colour
        AddSetting(new ColorSetting("Cursor", "Cursor circle colour", 1.0f, 1.0f, 1.0f, 1.0f));
        // [7] glow colour (when Glow is enabled)
        AddSetting(new ColorSetting("Glow Color", "Outer glow colour", 0.3f, 0.6f, 1.0f, 0.5f));
        // [8] cursor circle size (relative to panel)
        AddSetting(new NumberSetting("Cursor Size", "Size of the cursor circle", 0.08f, 0.02f, 0.25f, 0.01f));
        // [9] border thickness
        AddSetting(new NumberSetting("Border Thickness", "Thickness of the border outline", 1.5f, 0.5f, 5.0f, 0.1f));
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnRender() override;

    struct CircleTrail {
        float x, y, opacity;
    };

    std::vector<CircleTrail> trails;

    float mDeltaX = 0.0f;   // accumulated rotation delta (yaw)
    float mDeltaY = 0.0f;   // accumulated rotation delta (pitch)
    float mCursorX = 0.0f;  // smoothed cursor offset within the panel
    float mCursorY = 0.0f;
    Vec2 mPrevRot;          // previous frame's rotation (for delta computation)
    bool mHasPrev = false;
};
