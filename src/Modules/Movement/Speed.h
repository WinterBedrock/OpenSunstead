#pragma once
#include "../Module.h"

namespace SDK { class Actor; }

// Speed — increases the local player's horizontal movement speed.
//
// Vanilla model (rewritten): the old implementation RESCALED the existing
// velocity vector, which lagged the input direction and never matched vanilla
// movement (the user's complaint). We now REBUILD the horizontal velocity each
// tick from the player's YAW + live WASD input, exactly like the game's own
// move solver — ported from its MathUtils::getMotion +
// getRotationKeyOffset. The BASE speed is the player's RAW movement-speed
// attribute (Actor::getMovementSpeed), so Speed/Slowness effects scale
// naturally; it falls back to a proven vanilla sprint-ground constant if the
// attribute read is unavailable. This produces natural, vanilla-accurate
// movement instead of the compounding/lagging velocity multiply.
class Speed : public Module {
public:
    Speed() : Module("Speed", "Increases movement speed", Category::Movement, 0) {
        AddSetting(new EnumSetting("Type", "Speed mode", 0, { "Vanilla", "Bypass", "BunnyHop", "Flareon", "SprintHop", "Custom", "Flareon Fast" })); // [0] APPENDED modes keep 0-5 stable
        AddSetting(new NumberSetting("Multiplier", "Target ground-speed multiplier", 1.4f, 1.0f, 3.0f, 0.05f));   // [1]
        AddSetting(new BoolSetting("Strafe", "Move in the diagonal/strafe input direction (A/D and combos), not just straight ahead", true)); // [2]
        AddSetting(new BoolSetting("Use Player Speed", "Base the speed on the player's RAW movement-speed attribute (scales with Speed/Slowness effects). If off, uses a fixed vanilla ground-speed baseline.", true)); // [3]
        // Bypass mode knobs (Type = Bypass): keep the SERVER-visible displacement
        // inside what vanilla-accelerated movement can produce, so common
        // speed-checks (displacement-per-tick vs predicted max) stay satisfied.
        AddSetting(new NumberSetting("Bypass Mult", "Effective multiplier in Bypass mode (server-safe band, ~1.1-1.6)", 1.35f, 1.0f, 1.6f, 0.05f)); // [4]
        AddSetting(new BoolSetting("Jump Boost", "Auto-hop while moving in Bypass mode (jump-boost band tolerates more displacement than ground ticks)", false)); // [5]
        // APPENDED (Apollo-style hop modes): Height = jump/hold height impulse
        // (/10, vanilla impulse is 0.42), Friction = per-tick air-speed keep
        // factor (/10, 10 = no decay). Existing indexes above never move.
        AddSetting(new NumberSetting("Height", "Hop height impulse x10 (Apollo BunnyHop/Flareon/Custom)", 4.2f, 0.0f, 7.0f, 0.1f)); // [6]
        AddSetting(new NumberSetting("Friction", "Air speed kept per tick x10 (10 = none, Apollo SprintHop/Custom)", 10.0f, 0.0f, 10.0f, 0.1f)); // [7]
        AddSetting(new BoolSetting("Fast Fall", "Pull down faster once falling (Flareon-style)", false)); // [8]
        AddSetting(new NumberSetting("Fall Speed", "Fast-fall pull multiplier", 1.75f, 1.0f, 5.0f, 0.05f)); // [9]
        // APPENDED: knockback boost (Apollo DamageBoost without packet hooks —
        // a sudden horizontal velocity gain is the knockback signature).
        AddSetting(new BoolSetting("Damage Boost", "Short speed burst on knockback", false)); // [10]
        AddSetting(new NumberSetting("Boost Speed", "Boost multiplier while boosted", 1.5f, 1.0f, 3.0f, 0.1f)); // [11]
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnTick() override;
    void OnPacketSend(SDK::Packet* packet, int packetId, bool& cancel) override;

    int   GetType()           const { return static_cast<EnumSetting*>(mSettings[0])->mValue; }
    float GetMultiplier()     const { return GetType() == 1
                                            ? static_cast<NumberSetting*>(mSettings[4])->mValue
                                            : static_cast<NumberSetting*>(mSettings[1])->mValue; }
    bool  GetStrafe()         const { return static_cast<BoolSetting*>(mSettings[2])->mValue; }
    bool  GetUsePlayerSpeed() const { return static_cast<BoolSetting*>(mSettings[3])->mValue; }
    bool  GetJumpBoost()      const { return static_cast<BoolSetting*>(mSettings[5])->mValue; }
    float GetHeight()         const { return static_cast<NumberSetting*>(mSettings[6])->mValue; }
    float GetFriction()       const { return static_cast<NumberSetting*>(mSettings[7])->mValue; }
    bool  GetFastFall()       const { return static_cast<BoolSetting*>(mSettings[8])->mValue; }
    float GetFallSpeed()      const { return static_cast<NumberSetting*>(mSettings[9])->mValue; }
    bool  GetDamageBoost()    const { return static_cast<BoolSetting*>(mSettings[10])->mValue; }
    float GetBoostSpeed()     const { return static_cast<NumberSetting*>(mSettings[11])->mValue; }

private:
    void tickVanilla(SDK::Actor* lp);
    // Bypass: vanilla-shaped sprint+hop movement with a clamped multiplier and
    // sprint flags asserted so server-side motion prediction stays in band.
    void tickBypass(SDK::Actor* lp);
    // Apollo-style hop modes (no dead signatures: pure velocity + yaw math).
    void tickBunnyHop(SDK::Actor* lp);
    void tickFlareon(SDK::Actor* lp);
    void tickSprintHop(SDK::Actor* lp);
    void tickCustom(SDK::Actor* lp);
    void tickFlareonFast(SDK::Actor* lp);
    // Shared horizontal driver for the hop modes: rebuild velocity in the live
    // WASD direction at `target` blocks/tick. Returns false when there is no
    // input or no state to write (caller keeps vanilla behaviour then).
    bool driveHorizontal(SDK::Actor* lp, float target);
    static bool readWASD(bool& w, bool& a, bool& s, bool& d);
    long long mLastTick = -1;
    // Ground->air edge latch for the PAI jump-sync (per outgoing PAI packet).
    bool mWasOnGround = false;
    unsigned long long mLastHopMs = 0;   // hop-mode jump throttle
    bool mFallen = false;                 // fast-fall one-shot latch
    bool mHeightApplied = false;          // custom-mode air impulse latch
    float mCurrentSpeed = 0.0f;           // Flareon/SprintHop sustained speed
    float mLastSpeed = 0.0f;
    float mPrevSpeedMag = 0.0f;           // knockback spike detector
    unsigned long long mBoostUntilMs = 0;
    float mBoostMult = 1.0f;              // live damage-boost multiplier
    bool mTouchedTimer = false;
};
