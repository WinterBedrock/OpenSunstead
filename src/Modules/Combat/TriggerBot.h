#pragma once
#include "../Module.h"

// TriggerBot — automatically attacks the player you are looking at, like
// Solstice's TriggerBot (which checks the crosshair HitResult for an ENTITY and
// calls gameMode->attack). This client's HitResult does not expose the hit
// entity pointer, so we reproduce the same behaviour geometrically: if a player
// is inside a small angular window around the crosshair (i.e. you are aiming at
// them) and within reach, we swing + attack via the SAME GameMode vtable[15]
// attack path Aura uses. APS is randomized between Min/Max like Solstice.
class TriggerBot : public Module {
public:
    TriggerBot() : Module("TriggerBot", "Attacks the player you are aiming at", Category::Combat, 0) {
        AddSetting(new NumberSetting("Range", "Max reach to the target (blocks)", 4.0f, 2.0f, 8.0f, 0.1f));       // [0]
        AddSetting(new NumberSetting("FOV", "Crosshair angle (deg) that counts as 'aiming at'", 4.0f, 1.0f, 30.0f, 0.5f)); // [1]
        AddSetting(new NumberSetting("APS Min", "Minimum attacks per second", 10.0f, 1.0f, 20.0f, 1.0f));         // [2]
        AddSetting(new NumberSetting("APS Max", "Maximum attacks per second", 14.0f, 1.0f, 20.0f, 1.0f));         // [3]
        AddSetting(new NumberSetting("Delay", "Extra delay (ms) after you start aiming before the first hit", 0.0f, 0.0f, 500.0f, 10.0f)); // [4]
        AddSetting(new BoolSetting("Debug", "Print trigger diagnostics to the console", false));                 // [5]
    }

    void OnUpdate() override;

    float GetRange()  const { return static_cast<NumberSetting*>(mSettings[0])->mValue; }
    float GetFOV()    const { return static_cast<NumberSetting*>(mSettings[1])->mValue; }
    float GetAPSMin() const { return static_cast<NumberSetting*>(mSettings[2])->mValue; }
    float GetAPSMax() const { return static_cast<NumberSetting*>(mSettings[3])->mValue; }
    float GetTargetDelay() const { return static_cast<NumberSetting*>(mSettings[4])->mValue; }
    bool  GetDebug()  const { return static_cast<BoolSetting*>(mSettings[5])->mValue; }

private:
    unsigned long long mLastAttack = 0;
    unsigned long long mAimingSince = 0; // when we first started aiming at a player
    long long mLastAttackTick = -1;
};
