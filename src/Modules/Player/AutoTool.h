#pragma once
#include "../Module.h"

// AutoTool — automatically switches the held hotbar slot to the most efficient
// tool for the block the crosshair is on.
//
// Each frame (while mining, by default) it:
//   1. reads the Level's ray-cast HitResult; acts only when it is a BLOCK hit,
//   2. resolves that block's identifier via BlockSource -> Block -> BlockLegacy,
//   3. classifies the block to its ideal tool family (pickaxe / axe / shovel /
//      sword / shears),
//   4. scans the hotbar (slots 1-9) for the best-tier tool of that family and,
//   5. switches to it via a MobEquipment hotbar swap (the same server-synced
//      slot path Scaffold/PearlTrigger use) so the server applies the correct
//      break speed.
//
// "Switch Back" restores the slot held before the switch once the player stops
// mining, so the tool swap is transparent.
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
