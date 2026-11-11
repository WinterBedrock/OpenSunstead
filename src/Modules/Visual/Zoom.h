#pragma once
#include "../Module.h"

class Zoom : public Module {
public:
    Zoom() : Module("Zoom", "Zooms in when key is held", Category::Visual, 0x43) {
        AddSetting(new NumberSetting("Modifier", "Zoom strength (higher = more zoom, negative = wider FOV)", 15.0f, -10.0f, 25.0f, 1.0f));
        AddSetting(new BoolSetting("Smooth", "Animate zoom in/out", true));
        AddSetting(new EnumSetting("Bind Type", "How the keybind works", 1, {"Toggle", "Hold"}));
        canBind = true;
        mHoldMode = true;
    }
    void OnTick() override;
    void OnRender() override;
    // Does NOT snap the zoom: the release animation in OnRender finishes it.
    void OnDisable() override;

    float mCurrentFocal = 1.0f;
    bool mShouldZoom = false;
};
