#pragma once
#include "../Module.h"

class GhostHand : public Module {
public:
    GhostHand() : Module("GhostHand", "Hit players and open containers through blocks", Category::Player, 0) {
        AddSetting(new NumberSetting("Reach", "How far through blocks the ghost ray reaches", 4.5f, 1.0f, 6.0f, 0.1f));
        AddSetting(new BoolSetting("Entities", "Left click hits players through blocks", true));
        AddSetting(new BoolSetting("Chests", "Right click opens containers through blocks", true));
        canBind = false;
    }
    void OnEnable() override;
    // Called from the shared ClientInput tick hook, right after the original
    // (installed by Sprint; OnEnable makes sure it exists).
    static void AfterInputTick();
};
