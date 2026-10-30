#pragma once
#include "../Module.h"

// FastEat — accelerates eating (and any held-item use) by draining the local
// player's remaining-use-duration counter faster than vanilla.
//
// Reversed on 1.26.50.04: the counter is NOT a field on the Actor anymore. It
// lives in the entt component ItemInUseComponent (FNV-1a 32 hash 0x5B90AEF),
// element stride 16 bytes: { SharedPtr<ItemStack> item @ +0x0; int32
// remainingTicks @ +0x8 }. Mob::getItemUseDuration (vtable[0x93],
// sub_1401EDE80) returns exactly that int, and Mob::startUsingItem writes the
// item's max use duration into it — it counts DOWN one tick per game tick and
// the use finishes when it reaches 0. The previous build scanned the Actor
// object itself (lp+0x600..0xA00), which is why it never found anything.
//
// The module replicates the game's own component access (registry descriptor
// chain + entt sparse set) byte-for-byte, then burns (Speed - 1) extra ticks
// per game tick, clamped at >= 1 so the game's own finish path still runs.
class FastEat : public Module {
public:
    FastEat() : Module("FastEat", "Eat and use items faster", Category::Player, 0) {
        // Fine-grained multiplier: 1.00 = vanilla, 1.25 = slightly faster,
        // 2 = twice as fast, 32 = effectively instant. Fractional speeds burn
        // a fractional number of ticks per game tick (accumulated), so every
        // step of the slider is a real, observable difference.
        AddSetting(new NumberSetting("Speed", "Eating speed multiplier (1.00 = vanilla)", 2.0f, 1.0f, 32.0f, 0.25f)); // [0]
        AddSetting(new BoolSetting("Debug", "Log the counter drain to logs.txt", false));                          // [1]
    }

    void OnUpdate() override;   // game thread (hkUpdatePlayer) — same frame the counter ticks in
    void OnEnable() override;
    void OnDisable() override;

private:
    long long mLastTick = -1;   // last game tick we burned on
    float mBurnCarry = 0.0f;    // fractional leftover burn for sub-tick speeds
};
