#pragma once
#include "../Module.h"

namespace SDK { class Actor; }

// Switcher - server-visible weapon swap around clicks and hits
class Switcher : public Module {
public:
    Switcher();

    void OnEnable() override;
    void OnDisable() override;
    // Game-tick update: fresh LMB press -> swap in; release + hold-time
    // timeout -> restore. Runs on the game update thread.
    void OnUpdate() override;

    // Called from the Actor::attack hook, BEFORE the original runs
    void OnLocalAttack(SDK::Actor* lp);

private:
    // Weapon pick: 0 = Swords, 1 = Swords+Axes.
    int  GetWeaponMode() const { return static_cast<EnumSetting*>(mSettings[0])->mValue; }
    int  GetHoldMs() const { return static_cast<NumberSetting*>(mSettings[1])->mValue; }
    bool GetDebug() const { return static_cast<BoolSetting*>(mSettings[2])->mValue; }

    void Equip(SDK::Actor* lp);          // swap to the best weapon if not held
    void RestoreNow(SDK::Actor* lp);     // give the previous slot back

    bool mLmbDown = false;               // previous tick's LMB state (edge detect)
    bool mActive = false;                // a swap is in flight
    int  mRestoreSlot = -1;              // slot to restore when done
    unsigned long long mLastAttackMs = 0;
};
