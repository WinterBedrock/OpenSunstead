#pragma once
#include "../Module.h"

class FastMine : public Module {
public:
    FastMine() : Module("FastMine", "Multiplies the vanilla block-break rate", Category::Player, 0) {
        AddSetting(new NumberSetting("Multiplier", "Break rate multiplier (1.0 = vanilla)", 2.0f, 0.1f, 50.0f, 0.1f));
        canBind = true;
    }

    float GetMultiplier() const {
        return static_cast<NumberSetting*>(mSettings[0])->mValue;
    }
};
