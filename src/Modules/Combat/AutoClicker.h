#pragma once
#include "../Module.h"
#include <cstdint>

// AutoClicker — simulated combat clicks.
//
// LEFT mode drives the game's combat primitives directly (never touches the
// system mouse), exactly like TriggerBot:
//
//   * crosshair on an ENTITY -> Actor::swing(Attack) + GameMode::attack(target)
//   * crosshair on air/block -> Actor::swing(Attack) only, which is the vanilla
//     "missed swing" (the server sees the arm-swing animation and, when the
//     option is on, the MISSED_SWING AuthInput flag)
//
// RIGHT mode injects REAL right mouse clicks (SendInput RMB down/up) so the
// game's own input path performs the use — indistinguishable from a physical
// click, which is the point (bridging/place spam reads as a human clicking).
// The press is released on a later frame so the game's input poll cannot miss
// it, and only while the in-game HUD is up.
class AutoClicker : public Module {
public:
    AutoClicker() : Module("AutoClicker", "Automatically clicks for you", Category::Combat, 0) {
        AddSetting(new EnumSetting("Click Mode", "Which button to simulate", 0, {"Left", "Right"}));               // [0]
        AddSetting(new BoolSetting("Hold", "Only click while the mouse button is held", true));                    // [1]
        AddSetting(new BoolSetting("Randomize CPS", "Pick a fresh CPS between Min/Max for every click", true));    // [2]
        AddSetting(new NumberSetting("CPS", "Clicks per second", 10.0f, 1.0f, 60.0f, 1.0f));                       // [3]
        AddSetting(new NumberSetting("CPS Min", "Minimum randomized CPS", 8.0f, 1.0f, 60.0f, 1.0f));               // [4]
        AddSetting(new NumberSetting("CPS Max", "Maximum randomized CPS", 14.0f, 1.0f, 60.0f, 1.0f));              // [5]
        AddSetting(new BoolSetting("Debug", "Debug", false));                                                      // [6]
    }

    void OnTick() override;
    void OnDisable() override;
    void OnPacketSend(class SDK::Packet* packet, int packetId, bool& cancel) override;
    void UpdateSettingsVisibility() override;

    int   GetClickMode() const { return static_cast<EnumSetting*>(mSettings[0])->mValue; }
    bool  GetHold()      const { return static_cast<BoolSetting*>(mSettings[1])->mValue; }
    bool  GetRandomize() const { return static_cast<BoolSetting*>(mSettings[2])->mValue; }
    float GetCps()       const { return static_cast<NumberSetting*>(mSettings[3])->mValue; }
    float GetCpsMin()    const { return static_cast<NumberSetting*>(mSettings[4])->mValue; }
    float GetCpsMax()    const { return static_cast<NumberSetting*>(mSettings[5])->mValue; }
    // Attack-entities and swing-at-air behaviour is ALWAYS on (settings
    // removed): a crosshair hit attacks the target, a miss swings like vanilla.
    static constexpr bool GetAttackEntities() { return true; }
    static constexpr bool GetSwingAtAir() { return true; }
    bool  GetDebug()     const { return static_cast<BoolSetting*>(mSettings[6])->mValue; }

private:
    uint64_t  mNextClickMs = 0;   // wall-clock gate (CPS)
    long long mLastLeftTick = -1; // one attack/swing per game tick
    // In-flight synthetic right press (Right mode): DOWN was sent, UP is due
    // at this wall-clock time.
    uint64_t  mRightUpAtMs = 0;
    bool      mRightDownSent = false;
    // Set when a left click swung at nothing; consumed by the next outgoing
    // PlayerAuthInput so the server sees a vanilla missed swing.
    bool      mPendingMissedSwing = false;
};
