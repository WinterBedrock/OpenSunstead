#pragma once
#include "../Module.h"

class AimAssist : public Module {
public:
    AimAssist() : Module("AimAssist", "Smoothly aims your camera at the nearest player", Category::Combat, 0) {
        AddSetting(new NumberSetting("Range", "Target search radius in blocks", 6.0f, 2.0f, 12.0f, 0.5f));
        AddSetting(new NumberSetting("Smooth", "Higher = slower/smoother aim", 8.0f, 1.0f, 30.0f, 0.5f));
        AddSetting(new NumberSetting("FOV", "Max angle (deg) from crosshair to engage", 90.0f, 10.0f, 180.0f, 5.0f));
        AddSetting(new EnumSetting("Aim Point", "Hitbox region to aim at", 1, { "Legs", "Center", "Head" }));
    }

    void OnUpdate() override;
    void OnDisable() override;

    float GetRange()  const { return static_cast<NumberSetting*>(mSettings[0])->mValue; }
    float GetSmooth() const { return static_cast<NumberSetting*>(mSettings[1])->mValue; }
    float GetFOV()    const { return static_cast<NumberSetting*>(mSettings[2])->mValue; }
    int   GetAimPoint() const { return static_cast<EnumSetting*>(mSettings[3])->mValue; } // 0=Legs 1=Center 2=Head

private:
    unsigned long long mLastAimMs = 0;
};