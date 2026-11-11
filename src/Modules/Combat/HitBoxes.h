#pragma once

#include "../Module.h"
#include "../../SDK/Types.h"
#include <unordered_map>

namespace SDK { class Actor; class Level; }

class HitBoxes : public Module {
public:
    HitBoxes();

    void OnTick() override;
    void OnDisable() override;

private:
    int GetTargetMode() const { return static_cast<EnumSetting*>(mSettings[0])->mValue; }

    NumberSetting* widthSetting = nullptr;
    NumberSetting* heightSetting = nullptr;
    SDK::Level* trackedLevel = nullptr;
    std::unordered_map<SDK::Actor*, AABBShapeComponent> originalBoxes;
};
