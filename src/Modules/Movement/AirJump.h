#pragma once
#include "../Module.h"

namespace SDK { class Actor; }

// todo bypass :)
class AirJump : public Module {
public:
    AirJump() : Module("AirJump", "Jump in mid-air (forces the on-ground flag)", Category::Movement, 0) {}
    void OnTick() override;

    static bool ForceOnGround(SDK::Actor* lp);
};
