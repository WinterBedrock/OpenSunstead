#pragma once
#include "../Module.h"

class Fullbright : public Module {
public:
    Fullbright() : Module("Fullbright", "Makes dark areas bright by forcing gamma", Category::Visual, 0) {
        AddSetting(new NumberSetting("Gamma", "Forced brightness gamma", 12.0f, 1.0f, 30.0f, 0.5f));
    }

    float GetGamma() const;
};
