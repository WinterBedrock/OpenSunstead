#pragma once
#include "../Module.h"

class Zoom : public Module {
public:
    // Modifier semantics (see Zoom.cpp): 0 = normal FOV, positive = zoom in
    // (focal multiplier), NEGATIVE = FOV wider than normal (mapped onto (0,1)
    // by 1/(1-v)), so the slider literally runs from negative values up — the
    // weakest setting widens the FOV instead of barely zooming.
    Zoom() : Module("Zoom", "Zooms in when key is held", Category::Visual, 0x43) {
        AddSetting(new NumberSetting("Modifier", "Zoom strength (higher = more zoom, negative = wider FOV)", 15.0f, -10.0f, 25.0f, 1.0f));
        AddSetting(new BoolSetting("Smooth", "Animate zoom in/out", true));
        AddSetting(new EnumSetting("Bind Type", "How the keybind works", 1, {"Toggle", "Hold"}));
        canBind = true;
        mHoldMode = true;
    }
    void OnTick() override;
    // Release animation: the zoom-OUT after the module is disabled (key
    // release / toggle off) glides back fast instead of snapping. OnTick
    // stops running the moment the module disables, so the exit is finished
    // here — ModuleManager renders Zoom even while disabled for exactly
    // this reason (same pattern as ClickGUI/TabList close animations).
    void OnRender() override;
    // Does NOT snap the zoom: the release animation in OnRender finishes it.
    void OnDisable() override;

    // Animated state lives in FOCAL space (the actual projection scale),
    // never in modifier space: lerping the modifier toward 0 passes through
    // the (0..1) band where the focal mapping explodes into a WIDER-than-
    // normal FOV — that half-second of "everything far away" on release.
    float mCurrentFocal = 1.0f;
    bool mShouldZoom = false;
};
