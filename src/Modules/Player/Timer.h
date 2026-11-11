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

    static bool SetTps(float tps);
};
