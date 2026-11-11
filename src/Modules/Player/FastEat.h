#pragma once
#include "../Module.h"

class FastEat : public Module {
public:
    FastEat() : Module("FastEat", "Eat and use items faster", Category::Player, 0) {
        AddSetting(new NumberSetting("Speed", "Eating speed multiplier (1.00 = vanilla)", 2.0f, 1.0f, 32.0f, 0.25f));
    }

    void OnUpdate() override;   // game thread (hkUpdatePlayer) — same frame the counter ticks in
    void OnEnable() override;
    void OnDisable() override;

private:
    long long mLastTick = -1;   // last game tick we burned on
    float mBurnCarry = 0.0f;    // fractional leftover burn for sub-tick speeds
};
