#pragma once
#include "../Module.h"

class AimBot : public Module {
public:
    AimBot() : Module("AimBot", "Snaps your camera directly onto the nearest player", Category::Combat, 0) {
        AddSetting(new NumberSetting("Range", "Target search radius in blocks", 5.0f, 2.0f, 12.0f, 0.5f));       // [0]
        AddSetting(new NumberSetting("Smooth", "1 = fastest lock (still smooth, no tearing), higher = softer/slower", 2.0f, 1.0f, 20.0f, 0.5f)); // [1]
        AddSetting(new NumberSetting("FOV", "Max angle (deg) from crosshair to engage", 180.0f, 10.0f, 360.0f, 5.0f)); // [2]
        AddSetting(new EnumSetting("Aim Point", "Hitbox region to aim at", 2, { "Legs", "Center", "Head" }));    // [3]
        AddSetting(new BoolSetting("Only While Clicking", "Only aim while holding left mouse", false));          // [4]
    }

    void OnUpdate() override;
    void OnDisable() override;

    float GetRange()   const { return static_cast<NumberSetting*>(mSettings[0])->mValue; }
    float GetSmooth()  const { return static_cast<NumberSetting*>(mSettings[1])->mValue; }
    float GetFOV()     const { return static_cast<NumberSetting*>(mSettings[2])->mValue; }
    int   GetAimPoint() const { return static_cast<EnumSetting*>(mSettings[3])->mValue; } // 0=Legs 1=Center 2=Head
    bool  GetOnlyClicking() const { return static_cast<BoolSetting*>(mSettings[4])->mValue; }

private:
    unsigned long long mLastAimMs = 0;
};
