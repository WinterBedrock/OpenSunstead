#pragma once
#include "../Module.h"

class AutoTool : public Module {
public:
    AutoTool() : Module("AutoTool", "Switches to the most efficient tool for the targeted block", Category::Player, 0) {
        AddSetting(new BoolSetting("Only While Mining", "Only switch while the attack/destroy button is held (recommended). Off = switch whenever looking at a block.", true)); // [0]
        AddSetting(new BoolSetting("Switch Back", "Return to the previously-held slot when you stop mining", true)); // [1]
        AddSetting(new BoolSetting("Prefer Speed", "Rank by raw mining speed (gold highest) instead of tier (netherite highest)", false)); // [2]
        AddSetting(new BoolSetting("Debug", "Print tool-selection diagnostics to the console", false)); // [3]
    }

    void OnUpdate() override;
    void OnDisable() override;

    bool GetOnlyWhileMining() const { return static_cast<BoolSetting*>(mSettings[0])->mValue; }
    bool GetSwitchBack()      const { return static_cast<BoolSetting*>(mSettings[1])->mValue; }
    bool GetPreferSpeed()     const { return static_cast<BoolSetting*>(mSettings[2])->mValue; }
    bool GetDebug()           const { return static_cast<BoolSetting*>(mSettings[3])->mValue; }

private:
    bool mSwitched = false; // true while we hold a slot we switched to
    int  mPrevSlot = -1;    // slot held before our first switch
};
