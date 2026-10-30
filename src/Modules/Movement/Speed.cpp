#include "Speed.h"
#include "../../Utils/MCSDK.h"
#include "../Player/Timer.h"
#include "../Setting.h"
#include <cmath>
#include <Windows.h>

// Vanilla-style movement: rebuild the horizontal velocity from the player's YAW
// + live WASD input each tick, matching the game's own move solver (ported from
// its MathUtils::getMotion + getRotationKeyOffset). This replaces the old
// "rescale the existing velocity" logic, which lagged the input direction and
// never felt vanilla.
//
// BASE ground speed comes from the player's RAW movement-speed attribute
// (Actor::getMovementSpeed) so Speed/Slowness effects scale naturally. Vanilla
// base attribute is ~0.10, which corresponds to a sprint-ground speed of
// ~0.2873 blocks/tick, giving an attr->ground factor of ~2.873. If the
// attribute read is unavailable (unverified id, etc.) we fall back to that
// proven ground-speed constant so movement never breaks.
void Speed::tickVanilla(SDK::Actor* lp) {
    // Live WASD. Project convention: hardcoded keys (see InventoryMove /
    // Keystrokes), which ignores custom keybinds but matches the rest of the
    // codebase.
    bool w = (GetAsyncKeyState('W') & 0x8000) != 0;
    bool a = (GetAsyncKeyState('A') & 0x8000) != 0;
    bool s = (GetAsyncKeyState('S') & 0x8000) != 0;
    bool d = (GetAsyncKeyState('D') & 0x8000) != 0;
    // No movement input -> do NOT touch velocity, let vanilla friction/decel run
    // (the game handles stopping). Overriding here caused the non-vanilla feel.
    if (!(w || a || s || d)) return;

    auto* stateVec = lp->getStateVector();
    if (!stateVec) return;
    auto* rc = lp->getRotation();
    if (!rc) return;
    float yaw = rc->rotation.y;

    float mult = GetMultiplier();
    if (mult <= 0.0f) return;

    bool allowStrafe = GetStrafe();

    // Input-relative yaw offset: which way, relative to facing, the pressed keys
    // point. Same key-offset table Speed and Scaffold share.
    float off;
    if      (w && a && allowStrafe) off = -45.0f;
    else if (w && d && allowStrafe) off =  45.0f;
    else if (s && a && allowStrafe) off = -135.0f;
    else if (s && d && allowStrafe) off =  135.0f;
    else if (w)                     off =  0.0f;
    else if (a && allowStrafe)      off = -90.0f;
    else if (s)                     off = -180.0f;
    else if (d && allowStrafe)      off =  90.0f;
    else                            return; // only strafe keys while Strafe is off -> nothing to do

    // Base ground speed from the RAW movement-speed attribute when available,
    // else the proven vanilla sprint-ground constant.
    const float kBaseGroundSpeed = 0.2873f;                 // vanilla sprint-jump ground/tick
    const float kAttrToGround    = kBaseGroundSpeed / 0.1f; // ~2.873 attribute -> ground factor
    float ground = kBaseGroundSpeed;
    if (GetUsePlayerSpeed()) {
        auto raw = lp->getMovementSpeed();
        if (raw.has_value()) ground = raw.value() * kAttrToGround;
    }
    float target = ground * mult;

    // Rebuild horizontal velocity in the input direction. cos(yaw+off+90) ==
    // -sin(yaw+off), sin(yaw+off+90) == cos(yaw+off): the same forward-vector
    // convention this codebase already uses in Scaffold. Vertical velocity (y)
    // is left untouched so jumping/falling stays vanilla.
    const float D2R = 0.01745329252f;
    float ang = (yaw + off + 90.0f) * D2R;
    stateVec->velocity.x = std::cos(ang) * target;
    stateVec->velocity.z = std::sin(ang) * target;
}

void Speed::OnEnable() {
    mWasOnGround = false;
    mLastHopMs = 0;
    mFallen = false;
    mHeightApplied = false;
    mCurrentSpeed = 0.0f;
    mLastSpeed = 0.0f;
    mPrevSpeedMag = 0.0f;
    mBoostUntilMs = 0;
    mBoostMult = 1.0f;
}

void Speed::OnDisable() {
    mWasOnGround = false;
    mFallen = false;
    mHeightApplied = false;
    mCurrentSpeed = 0.0f;
    mLastSpeed = 0.0f;
    mPrevSpeedMag = 0.0f;
    mBoostUntilMs = 0;
    mBoostMult = 1.0f;
    if (mTouchedTimer) {
        mTouchedTimer = false;
        Timer::SetTps(20.0f);
    }
}

bool Speed::readWASD(bool& w, bool& a, bool& s, bool& d) {
    w = (GetAsyncKeyState('W') & 0x8000) != 0;
    a = (GetAsyncKeyState('A') & 0x8000) != 0;
    s = (GetAsyncKeyState('S') & 0x8000) != 0;
    d = (GetAsyncKeyState('D') & 0x8000) != 0;
    return w || a || s || d;
}

// Shared horizontal driver: input-relative yaw rebuild at `target` b/tick.
// Same forward-vector convention as tickVanilla/tickBypass.
bool Speed::driveHorizontal(SDK::Actor* lp, float target) {
    bool w, a, s, d;
    if (!readWASD(w, a, s, d)) return false;
    auto* stateVec = lp->getStateVector();
    if (!stateVec) return false;
    auto* rc = lp->getRotation();
    if (!rc) return false;
    const float yaw = rc->rotation.y;
    const bool allowStrafe = GetStrafe();
    float off;
    if      (w && a && allowStrafe) off = -45.0f;
    else if (w && d && allowStrafe) off =  45.0f;
    else if (s && a && allowStrafe) off = -135.0f;
    else if (s && d && allowStrafe) off =  135.0f;
    else if (w)                     off =   0.0f;
    else if (a && allowStrafe)      off = -90.0f;
    else if (s)                     off = -180.0f;
    else if (d && allowStrafe)      off =  90.0f;
    else                            return false;
    const float D2R = 0.01745329252f;
    float ang = (yaw + off + 90.0f) * D2R;
    stateVec->velocity.x = std::cos(ang) * target;
    stateVec->velocity.z = std::sin(ang) * target;
    return true;
}

static float hopTarget(float mult) {
    if (mult < 1.0f) mult = 1.0f;
    if (mult > 3.0f) mult = 3.0f;
    return 0.2873f * mult;   // vanilla sprint-jump air speed, scaled
}

// Apollo "Bunny hop": hop on every ground tick, hold speed in the air.
void Speed::tickBunnyHop(SDK::Actor* lp) {
    bool w, a, s, d;
    if (!readWASD(w, a, s, d)) return;
    auto* stateVec = lp->getStateVector();
    if (!stateVec) return;
    const bool onGround = lp->getStatusFlag(ActorFlags::Standing) == 1;
    if (onGround && GetHeight() > 0.0f)
        stateVec->velocity.y = GetHeight() / 10.0f;   // Apollo: height/10
    driveHorizontal(lp, hopTarget(GetMultiplier()) * mBoostMult);
}

// Apollo "Flareon": jump on a 100 ms cadence, sustain air speed, fast-fall.
void Speed::tickFlareon(SDK::Actor* lp) {
    bool w, a, s, d;
    if (!readWASD(w, a, s, d)) return;
    auto* stateVec = lp->getStateVector();
    if (!stateVec) return;
    const unsigned long long now = GetTickCount64();
    const bool onGround = lp->getStatusFlag(ActorFlags::Standing) == 1;
    if (onGround) {
        if (now - mLastHopMs >= 100) {
            const float mag = std::sqrt(stateVec->velocity.x * stateVec->velocity.x +
                                        stateVec->velocity.z * stateVec->velocity.z);
            mCurrentSpeed = mag;
            stateVec->velocity.y = 0.42f;
            mLastHopMs = now;
        }
    } else {
        const float mag = std::sqrt(stateVec->velocity.x * stateVec->velocity.x +
                                    stateVec->velocity.z * stateVec->velocity.z);
        mCurrentSpeed = mag;
        driveHorizontal(lp, mCurrentSpeed * mBoostMult);
    }
    if (GetFastFall()) {
        if (stateVec->velocity.y >= -0.2f) mFallen = false;
        else if (!mFallen) { stateVec->velocity.y *= GetFallSpeed(); mFallen = true; }
    }
}

// Apollo "Sprint hop": forced sprint, 93 ms hop cadence, friction decay.
void Speed::tickSprintHop(SDK::Actor* lp) {
    lp->setSprinting(true);
    bool w, a, s, d;
    if (!readWASD(w, a, s, d)) return;
    auto* stateVec = lp->getStateVector();
    if (!stateVec) return;
    const unsigned long long now = GetTickCount64();
    const bool onGround = lp->getStatusFlag(ActorFlags::Standing) == 1;
    if (onGround) {
        if (now - mLastHopMs >= 93) {
            stateVec->velocity.y = 0.42f;
            mLastHopMs = now;
        }
    } else {
        const float mag = std::sqrt(stateVec->velocity.x * stateVec->velocity.x +
                                    stateVec->velocity.z * stateVec->velocity.z);
        if (std::fabs(mag) > mLastSpeed) mCurrentSpeed = mag;
        mLastSpeed = mag;
        mCurrentSpeed = mCurrentSpeed * (GetFriction() / 10.0f);
        driveHorizontal(lp, mCurrentSpeed * mBoostMult);
    }
    if (GetFastFall()) {
        if (stateVec->velocity.y >= -0.2f) mFallen = false;
        else if (!mFallen) { stateVec->velocity.y *= GetFallSpeed(); mFallen = true; }
    }
}

// Apollo "Flareon Fast": Flareon sustain + motion-table Y pull + timer
// stretched while falling (26) and vanilla otherwise (20).
void Speed::tickFlareonFast(SDK::Actor* lp) {
    tickFlareon(lp);
    auto* stateVec = lp->getStateVector();
    if (!stateVec) return;
    const float motion = std::round(stateVec->velocity.y * 1000.0f) / 1000.0f;
    static const float table[3] = { 0.248f, 0.126f, 0.005f };
    for (float m : table) {
        if (std::fabs(motion - m) < 0.0001f) { stateVec->velocity.y -= 0.04f; break; }
    }
    if (Timer::SetTps(stateVec->velocity.y < 0.0f ? 26.0f : 20.0f))
        mTouchedTimer = true;
}

// Apollo "Custom": jump, hold a height impulse once per airtime, friction decay.
void Speed::tickCustom(SDK::Actor* lp) {
    bool w, a, s, d;
    if (!readWASD(w, a, s, d)) return;
    auto* stateVec = lp->getStateVector();
    if (!stateVec) return;
    const unsigned long long now = GetTickCount64();
    const bool onGround = lp->getStatusFlag(ActorFlags::Standing) == 1;
    if (onGround) {
        if (now - mLastHopMs >= 100) {
            mCurrentSpeed = hopTarget(GetMultiplier());
            stateVec->velocity.y = 0.42f;
            mLastHopMs = now;
            mHeightApplied = false;
        }
    } else {
        if (!mHeightApplied) {
            stateVec->velocity.y = GetHeight() / 10.0f;
            mHeightApplied = true;
        } else {
            const float mag = std::sqrt(stateVec->velocity.x * stateVec->velocity.x +
                                        stateVec->velocity.z * stateVec->velocity.z);
            if (mag < mCurrentSpeed * (GetFriction() / 10.0f))
                mCurrentSpeed = mag;
            mCurrentSpeed *= GetFriction() / 10.0f;
        }
        driveHorizontal(lp, mCurrentSpeed * mBoostMult);
    }
    if (GetFastFall()) {
        if (stateVec->velocity.y >= -0.2f) mFallen = false;
        else if (!mFallen) { stateVec->velocity.y *= GetFallSpeed(); mFallen = true; }
    }
}

void Speed::OnTick() {
    if (!IsEnabled()) return;
    // WASD must only be read from the HUD screen — while the chat (or any
    // other screen) is open the same keys are TEXT INPUT and must never drive
    // movement.
    if (!SDK::isGameplayInputActive()) return;

    SDK::ClientInstance* cInst = SDK::ClientInstance::get();
    if (!cInst) return;
    SDK::Actor* lp = cInst->getLocalPlayer();
    if (!lp) return;

    int type = static_cast<EnumSetting*>(mSettings[0])->mValue;

    // Knockback boost (Apollo DamageBoost, packet-free): a sudden horizontal
    // velocity gain while airborne is the knockback signature — ride it for
    // ~590 ms like Apollo's DamageBoost window.
    mBoostMult = 1.0f;
    if (GetDamageBoost()) {
        auto* sv = lp->getStateVector();
        if (sv) {
            const float mag = std::sqrt(sv->velocity.x * sv->velocity.x +
                                        sv->velocity.z * sv->velocity.z);
            const unsigned long long now = GetTickCount64();
            const bool airborne = lp->getStatusFlag(ActorFlags::Standing) != 1;
            if (airborne && mag - mPrevSpeedMag > 0.12f)
                mBoostUntilMs = now + 590;
            mPrevSpeedMag = mag;
            if (now < mBoostUntilMs) mBoostMult = GetBoostSpeed();
        }
    }

    switch (type) {
        case 1: // Bypass
            tickBypass(lp);
            break;
        case 2: // BunnyHop (Apollo)
            tickBunnyHop(lp);
            break;
        case 3: // Flareon (Apollo)
            tickFlareon(lp);
            break;
        case 4: // SprintHop (Apollo)
            tickSprintHop(lp);
            break;
        case 5: // Custom (Apollo)
            tickCustom(lp);
            break;
        case 6: // Flareon Fast (Apollo)
            tickFlareonFast(lp);
            break;
        case 0: // Vanilla
        default:
            tickVanilla(lp);
            break;
    }
}

// Bypass mode: stay inside the displacement band that vanilla-accelerated
// movement can produce, so servers whose speed-check compares measured
// per-tick displacement against the predicted maximum stay satisfied.
//   * multiplier hard-capped at 1.6 (a sprint-jumping player already measures
//     ~1.3x on the ground band; 1.6 leaves headroom without standing out),
//   * the boost is applied ONLY while airborne (jump-ticks), ground ticks run
//     vanilla speed — the classic "hop speed" profile anticheats tolerate,
//   * auto-hop while moving to stay in the airborne band,
//   * sprint flags asserted so the server's own prediction sprints with us.
void Speed::tickBypass(SDK::Actor* lp) {
    bool w = (GetAsyncKeyState('W') & 0x8000) != 0;
    bool a = (GetAsyncKeyState('A') & 0x8000) != 0;
    bool s = (GetAsyncKeyState('S') & 0x8000) != 0;
    bool d = (GetAsyncKeyState('D') & 0x8000) != 0;
    const bool moving = w || a || s || d;
    if (!moving) return;   // no input -> vanilla friction handles stopping

    auto* stateVec = lp->getStateVector();
    if (!stateVec) return;

    // Assert sprint so the server's predicted speed matches ours.
    lp->setSprinting(true);

    // Auto-hop keeps us in the airborne band where more displacement is legal.
    const bool onGround = lp->getStatusFlag(ActorFlags::Standing) == 1;
    if (GetJumpBoost() && onGround) {
        stateVec->velocity.y = 0.42f;   // vanilla jump impulse
    }

    float mult = GetMultiplier();
    if (mult > 1.6f) mult = 1.6f;
    if (mult < 1.0f) mult = 1.0f;

    const bool airborne = !onGround;
    // Ground ticks stay ~vanilla: the server predicts a ground walk there (the
    // jump-sync bits below only fire on the hop), so this is where overspeed
    // actually gets flagged. The multiplier itself lives on AIR ticks, inside
    // the jump-arc band the predicted motion tolerates.
    const float base = airborne ? 0.2873f   // vanilla sprint-jump air speed
                                : 0.2195f;  // vanilla walk-ground speed
    float eff = mult;
    if (!airborne && eff > 1.1f) eff = 1.1f; // ground displacement ~vanilla
    float target = base * eff;

    // Tiny humanization jitter (about +-3 percent) so per-tick displacement
    // does not repeat with machine-regular spacing.
    const float jitter = 1.0f + ((float)((GetTickCount64() % 7) - 3) * 0.01f);
    target *= jitter;

    auto* rc = lp->getRotation();
    if (!rc) return;
    float yaw = rc->rotation.y;

    float off;
    if      (w && a) off = -45.0f;
    else if (w && d) off =  45.0f;
    else if (s && a) off = -135.0f;
    else if (s && d) off =  135.0f;
    else if (w)      off =   0.0f;
    else if (a)      off = -90.0f;
    else if (s)      off = -180.0f;
    else             off =  90.0f;

    const float D2R = 0.01745329252f;
    float ang = (yaw + off + 90.0f) * D2R;
    stateVec->velocity.x = std::cos(ang) * target;
    stateVec->velocity.z = std::sin(ang) * target;
}

void Speed::OnPacketSend(SDK::Packet* packet, int packetId, bool& /*cancel*/) {
    if (!IsEnabled() || !packet) return;
    if (GetType() != 1 || !GetJumpBoost()) return; // only the hop profile lies about jumping
    if (packetId != static_cast<int>(SDK::PacketID::PlayerAuthInput)) return;
    if (!sdk_safe_ptr(packet, 0x90)) return;
    // Same WASD gate as OnTick: typing in chat must not inject jump bits.
    if (!SDK::isGameplayInputActive()) { mWasOnGround = false; return; }

    SDK::ClientInstance* cInst = SDK::ClientInstance::get();
    if (!cInst) return;
    SDK::Actor* lp = cInst->getLocalPlayer();
    if (!lp) return;

    const bool w = (GetAsyncKeyState('W') & 0x8000) != 0;
    const bool a = (GetAsyncKeyState('A') & 0x8000) != 0;
    const bool s = (GetAsyncKeyState('S') & 0x8000) != 0;
    const bool d = (GetAsyncKeyState('D') & 0x8000) != 0;
    const bool moving = w || a || s || d;

    const int ground = lp->getStatusFlag(ActorFlags::Standing);
    const bool onGround = ground == 1;

    if (moving) {
        auto* pai = reinterpret_cast<SDK::PlayerAuthInputPacket*>(packet);
        pai->addInput(SDK::AuthInputAction::JUMPING |
                      SDK::AuthInputAction::WANT_UP |
                      SDK::AuthInputAction::JUMP_DOWN);
        if (!onGround && mWasOnGround)
            pai->addInput(SDK::AuthInputAction::START_JUMPING);
    }

    // Latch only on a definite read (-1 = component unreadable -> keep old).
    if (ground >= 0) mWasOnGround = onGround;
}
