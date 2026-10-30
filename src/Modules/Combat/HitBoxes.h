#pragma once

#include "../Module.h"

class HitBoxes : public Module {
public:
    HitBoxes();

    void OnTick() override;
    void OnDisable() override;

    NumberSetting* widthSetting = nullptr;
    NumberSetting* heightSetting = nullptr;

private:
    // Target filter dropdown. 0 = all entities (mobs, items, everything the
    // level lists), 1 = players only, 2 = players filtered through the
    // AntiBot module (bots excluded).
    int GetTargetMode() const { return static_cast<EnumSetting*>(mSettings[0])->mValue; }
};
