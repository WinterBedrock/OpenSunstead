#pragma once
#include "../Module.h"
#include "../Setting.h"

namespace SDK { class Actor; class Packet; }

// Fly — flight modes, creative baseline plus Apollo-style bypasses:
//   Creative — vanilla creative rebuild (zero + WASD/space/shift + glide).
//   Flareon  — timer-stretched hop sustain (Apollo Flareon, minus its packet
//              desync): sprint, 100 ms hop cadence, 0.9x air sustain.
//   Glide    — timer + pinned vertical ("silly"): fixed Y velocity while the
//              timer stretches ticks.
//   Funny    — hover with server-pinned Y (Apollo "funny"): vel.y = 0, gentle
//              0.307 air speed, and the outgoing PAI reports the take-off Y so
//              the server never sees the hover (pair with NoFall for landing).
//   AirWalk  — Apollo case 5: force the on-ground flag (shared AirJump
//              machinery) + creative-style glide, so movement integrates as a
//              ground walk while airborne.
// Fly Blink (any mode): swallow outgoing movement packets, freezing the
// server-side position while the client flies freely.
class Fly : public Module {
public:
    Fly() : Module("Fly", "Fly like in creative mode", Category::Movement, 0) {
        AddSetting(new NumberSetting("Horizontal Speed", "Horizontal flight speed", 1.0f, 0.2f, 15.0f, 0.05f)); // [0]
        AddSetting(new NumberSetting("Vertical Speed", "Vertical flight speed", 0.5f, 0.2f, 3.0f, 0.05f));      // [1]
        AddSetting(new NumberSetting("Glide Rate", "Downward drift while hovering (vanilla creative ~= -0.02)", -0.02f, -0.6f, 0.0f, 0.01f)); // [2]
        AddSetting(new BoolSetting("Dynamic", "Brief speed burst right after taking off", false));              // [3]
        AddSetting(new BoolSetting("Stop On Disable", "Zero velocity when the module turns off", true));         // [4]
        // APPENDED (indexes above never move).
        AddSetting(new EnumSetting("Mode", "Flight mode", 0,
            { "Creative", "Flareon", "Glide", "Funny", "AirWalk" }));                                            // [5]
        AddSetting(new NumberSetting("Timer Speed", "Game timer while Flareon/Glide run (20 = vanilla)", 5.0f, 1.0f, 30.0f, 0.5f)); // [6]
        AddSetting(new NumberSetting("Fixed Y", "Pinned vertical velocity for Glide mode", -0.2f, -1.0f, 1.0f, 0.05f)); // [7]
        AddSetting(new BoolSetting("Fly Blink", "Swallow outgoing movement packets (server-side freeze)", false)); // [8]
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnTick() override;
    void OnPacketSend(SDK::Packet* packet, int packetId, bool& cancel) override;

    float GetHSpeed() const { return static_cast<NumberSetting*>(mSettings[0])->mValue; }
    float GetVSpeed() const { return static_cast<NumberSetting*>(mSettings[1])->mValue; }
    float GetGlide()  const { return static_cast<NumberSetting*>(mSettings[2])->mValue; }
    bool  GetDynamic() const { return static_cast<BoolSetting*>(mSettings[3])->mValue; }
    bool  GetStopOnDisable() const { return static_cast<BoolSetting*>(mSettings[4])->mValue; }
    int   GetMode()   const { return static_cast<EnumSetting*>(mSettings[5])->mValue; }
    float GetTimerSpeed() const { return static_cast<NumberSetting*>(mSettings[6])->mValue; }
    float GetFixedY() const { return static_cast<NumberSetting*>(mSettings[7])->mValue; }
    bool  GetFlyBlink() const { return static_cast<BoolSetting*>(mSettings[8])->mValue; }

private:
    int mTicksEnabled = 0;
    float mStartY = 0.0f;
    unsigned long long mLastHopMs = 0;
    float mCurrentSpeed = 0.0f;
    float mLastSpeed = 0.0f;
    bool mTouchedTimer = false;

    void tickCreative(SDK::Actor* lp);
    void tickFlareon(SDK::Actor* lp);
    void tickGlide(SDK::Actor* lp);
    void tickFunny(SDK::Actor* lp);
    void tickAirWalk(SDK::Actor* lp);
    // Horizontal velocity rebuild in the live WASD direction at `target`.
    bool driveFly(SDK::Actor* lp, float target);
};
