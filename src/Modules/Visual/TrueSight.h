#pragma once
#include "../Module.h"

class TrueSight : public Module {
public:
    TrueSight() : Module("TrueSight", "Shows invisible actors the server hides from you", Category::Visual, 0) {
        canBind = true;
    }
    void OnTick() override;
    void OnDisable() override;
};
