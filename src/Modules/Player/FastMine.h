#pragma once
#include "../Module.h"

// FastMine — multiplies the vanilla block-break rate.
//
// Reverse-engineered on 1.26.42.01: GameMode::getDestroyRate(Block const&)
// (0x142D2EBE0, resolved by signature) returns the fraction of the block broken
// per GAME TICK for the held tool:
//
//     rate = toolSpeed / hardness / (canHarvest ? 30 : 100)
//            * 1.2^hasteAmplifier * 0.7^(miningFatigueAmplifier + 1)
//
// The client's continueDestroyBlock adds elapsed/0.05 * rate into its progress
// accumulator every tick (gm+0x24) and dispatches the finish dispatch at
// >= 0.99998999. Because the rate is a plain float return value, the entire
// vanilla pipeline — start, per-tick accumulate, finish dispatch, and every
// other consumer — is scaled by detouring this ONE function and multiplying
// its result. No memory patches, no accumulator edits: the outbound break
// transaction stays byte-for-byte vanilla, just sooner.
//
// The hook itself lives in GameHooks.cpp (MinHook, installed lazily next to the
// other game-function detours); this module only owns the multiplier setting.
class FastMine : public Module {
public:
    FastMine() : Module("FastMine", "Multiplies the vanilla block-break rate", Category::Player, 0) {
        AddSetting(new NumberSetting("Multiplier", "Break rate multiplier (1.0 = vanilla)", 2.0f, 0.1f, 50.0f, 0.1f));
        canBind = true;
    }

    float GetMultiplier() const {
        return static_cast<NumberSetting*>(mSettings[0])->mValue;
    }
};
