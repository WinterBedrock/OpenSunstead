#pragma once
#include "../Module.h"

class Timer : public Module {
public:
    Timer() : Module("Timer", "Changes the game tick speed", Category::Player, 0) {
        AddSetting(new NumberSetting("Speed", "Game tick speed (20.0 = normal)", 20.0f, 0.1f, 60.0f, 0.01f));
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnTick() override;

    // Shared timer-speed writer for hop/fly bypasses (Flareon-style modes).
    // SEH-guarded; false when no timer is reachable. Restoring vanilla (20)
    // is the caller's job on disable.
    static bool SetTps(float tps);
};
