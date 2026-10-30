#pragma once
#include "../Module.h"

// PlaceDelay — removes the delay the engine inserts between block placements
// while the RIGHT MOUSE BUTTON is HELD.
//
// THREE engine mechanisms pace vanilla hold-to-place (all reversed on
// 1.26.50.04, GameMode vtable 0x14E826560):
//
//   1. gm+0xA0 (UseRepeatIntervalMs, int64 ms, DEFAULT 250 from the GameMode
//      ctor sub_142598DB0) — the engine's own hold-repeat interval.
//      GameMode::startDestroyBlock (sub_14259A470) primes
//      gm+0x90 = now + 1000000 * gm[0xA0] on the first click, and every
//      repeat reschedules the same way. With 250 ms there, holding RMB places
//      at most ~4 blocks/sec — that gap is exactly "the delay between
//      placements while holding".
//   2. gm+0x88 (BuildCooldownLastNs) — buildBlock's own 100-300 ms gate,
//      rewritten after every successful place.
//   3. gm+0x90 (UseRepeatNextNs) — the repeat gate itself.
//
// The module now attacks all three:
//   * gm+0xA0 is forced to 1 ms while enabled (restored to 250 on disable) so
//     the ENGINE's own hold loop runs at tick rate instead of 4/s;
//   * the two stamps are zeroed every tick while RMB is held (the buildBlock /
//     continueDestroyBlock detours in GameHooks do the same around the vanilla
//     hold path);
//   * on top of that, a full game-native right-click is injected through
//     MouseDevice::feed (the exact primitive GameCore::handleMouseInput uses
//     for a real click) at the module's own rate — every injected click runs
//     the COMPLETE vanilla use pipeline (item use transaction, prediction,
//     packets), so placements fire at the configured rate regardless of any
//     pacing that survives inside the engine.
//
// "Click Delay" is the spacing between injected clicks: 0 means every frame,
// 50 ms (default) means one placement per game tick — the fastest rate a
// server processes anyway.
class PlaceDelay : public Module {
public:
    PlaceDelay();

    void OnEnable() override;
    void OnDisable() override;
    void OnTick() override;

    bool GetOnlyScaffold() const { return static_cast<BoolSetting*>(mSettings[0])->mValue; }
    float GetClickDelayMs() const { return static_cast<NumberSetting*>(mSettings[1])->mValue; }

private:
    void* mPatchedGm = nullptr;   // GameMode whose gm+0xA0 we forced (restore target)
    unsigned long long mLastClickMs = 0;
};
