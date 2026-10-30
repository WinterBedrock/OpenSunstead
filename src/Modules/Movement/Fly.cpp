#include "Fly.h"
#include "AirJump.h"
#include "../Player/Timer.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/Network/PacketTypes.h"
#include <cmath>
#include <Windows.h>

static constexpr float kPi = 3.141592653589793f;
// PlayerAuthInput mPos (same layout GameHooks uses for silent rotation).
static constexpr uintptr_t kPAI_mPos = 0x38;

void Fly::OnEnable() {
    mTicksEnabled = 0;
    mLastHopMs = 0;
    mCurrentSpeed = 0.0f;
    mLastSpeed = 0.0f;
    mTouchedTimer = false;
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    SDK::Actor* lp = ci ? ci->getLocalPlayer() : nullptr;
    mStartY = 0.0f;
    if (lp) {
        Vec3 p{};
        if (SDK::sehGetPos(lp, &p)) mStartY = p.y;
    }
}

void Fly::OnDisable() {
    if (mTouchedTimer) {
        mTouchedTimer = false;
        Timer::SetTps(20.0f);
    }
    if (!GetStopOnDisable()) return;
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci) return;
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp) return;
    auto* sv = lp->getStateVector();
    if (sv) sv->velocity = Vec3{0.f, 0.f, 0.f};
}

bool Fly::driveFly(SDK::Actor* lp, float target) {
    if (!SDK::isGameplayInputActive()) return false;
    const bool w = (GetAsyncKeyState('W') & 0x8000) != 0;
    const bool a = (GetAsyncKeyState('A') & 0x8000) != 0;
    const bool s = (GetAsyncKeyState('S') & 0x8000) != 0;
    const bool d = (GetAsyncKeyState('D') & 0x8000) != 0;
    const int strafe = (d ? 1 : 0) - (a ? 1 : 0);
    const int fwd    = (w ? 1 : 0) - (s ? 1 : 0);
    if (strafe == 0 && fwd == 0) return false;
    auto* sv = lp->getStateVector();
    if (!sv) return false;
    const float yaw = lp->getRot().y;
    const float angleDeg = std::atan2(static_cast<float>(strafe), static_cast<float>(fwd)) * (180.0f / kPi);
    const float calcYaw = (yaw + angleDeg + 90.0f) * (kPi / 180.0f);
    sv->velocity.x = std::cos(calcYaw) * target;
    sv->velocity.z = std::sin(calcYaw) * target;
    return true;
}

// Creative baseline: pure glide + WASD/space/shift rebuild.
void Fly::tickCreative(SDK::Actor* lp) {
    auto* sv = lp->getStateVector();
    if (!sv) return;
    const float glide = GetGlide();
    Vec3 vel{0.f, glide, 0.f};

    if (SDK::isGameplayInputActive()) {
        const bool up   = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
        const bool down = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

        float hSpeed = GetHSpeed();
        float vSpeed = GetVSpeed();
        if (GetDynamic() && up && mTicksEnabled < 10) {
            hSpeed = std::max(hSpeed * 2.0f, 2.0f);
            vSpeed = std::max(vSpeed * 2.0f, 1.0f);
        }

        if (up)   vel.y += vSpeed;
        if (down) vel.y -= vSpeed;

        const bool w = (GetAsyncKeyState('W') & 0x8000) != 0;
        const bool a = (GetAsyncKeyState('A') & 0x8000) != 0;
        const bool s = (GetAsyncKeyState('S') & 0x8000) != 0;
        const bool d = (GetAsyncKeyState('D') & 0x8000) != 0;
        const int strafe = (d ? 1 : 0) - (a ? 1 : 0);
        const int fwd    = (w ? 1 : 0) - (s ? 1 : 0);
        if (strafe != 0 || fwd != 0) {
            const float yaw = lp->getRot().y;
            const float angleDeg = std::atan2(static_cast<float>(strafe), static_cast<float>(fwd)) * (180.0f / kPi);
            const float calcYaw = (yaw + angleDeg + 90.0f) * (kPi / 180.0f);
            vel.x = std::cos(calcYaw) * hSpeed;
            vel.z = std::sin(calcYaw) * hSpeed;
        }
    }

    sv->velocity = vel;
}

// Apollo Flareon: stretched timer, hop cadence, sustained air speed.
void Fly::tickFlareon(SDK::Actor* lp) {
    auto* sv = lp->getStateVector();
    if (!sv) return;
    if (Timer::SetTps(GetTimerSpeed())) mTouchedTimer = true;
    lp->setSprinting(true);
    bool w = false, a = false, s = false, d = false;
    const bool moving = SDK::isGameplayInputActive() &&
        ((w = (GetAsyncKeyState('W') & 0x8000) != 0) ||
         (a = (GetAsyncKeyState('A') & 0x8000) != 0) ||
         (s = (GetAsyncKeyState('S') & 0x8000) != 0) ||
         (d = (GetAsyncKeyState('D') & 0x8000) != 0));
    (void)w; (void)a; (void)s; (void)d;
    if (!moving) return;
    const unsigned long long now = GetTickCount64();
    const bool onGround = lp->getStatusFlag(ActorFlags::Standing) == 1;
    if (onGround) {
        if (now - mLastHopMs >= 100) {
            mCurrentSpeed = 0.97f;
            mLastSpeed = 0.97f;
            sv->velocity.y = 0.42f;
            mLastHopMs = now;
        }
    } else {
        const float mag = std::sqrt(sv->velocity.x * sv->velocity.x +
                                    sv->velocity.z * sv->velocity.z);
        if (std::fabs(mag) > mLastSpeed) mCurrentSpeed = mag;
        mLastSpeed = mag;
        mCurrentSpeed = mCurrentSpeed * 0.9f;
        driveFly(lp, mCurrentSpeed);
    }
}

// Apollo "silly": stretched timer, pinned vertical, horizontal cruise.
void Fly::tickGlide(SDK::Actor* lp) {
    auto* sv = lp->getStateVector();
    if (!sv) return;
    if (Timer::SetTps(GetTimerSpeed())) mTouchedTimer = true;
    sv->velocity.y = GetFixedY();
    driveFly(lp, GetHSpeed());
    if (SDK::isGameplayInputActive()) {
        if ((GetAsyncKeyState(VK_SPACE) & 0x8000) != 0) sv->velocity.y += 0.3f;
        else if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) sv->velocity.y -= 0.3f;
    }
}

// Apollo "funny": hover with server-pinned Y (see OnPacketSend).
void Fly::tickFunny(SDK::Actor* lp) {
    auto* sv = lp->getStateVector();
    if (!sv) return;
    const bool onGround = lp->getStatusFlag(ActorFlags::Standing) == 1;
    if (!onGround) driveFly(lp, 0.307f);
    sv->velocity.y = 0.0f;
    if (SDK::isGameplayInputActive()) {
        if ((GetAsyncKeyState(VK_SPACE) & 0x8000) != 0) sv->velocity.y += 0.3f;
        else if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) sv->velocity.y -= 0.3f;
    }
}

// Apollo AirWalk: grounded flag + cruise + glide.
void Fly::tickAirWalk(SDK::Actor* lp) {
    AirJump::ForceOnGround(lp);
    auto* sv = lp->getStateVector();
    if (!sv) return;
    sv->velocity.y = GetGlide();
    driveFly(lp, GetHSpeed());
}

void Fly::OnTick() {
    if (!IsEnabled()) return;

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci) return;
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp) return;
    ++mTicksEnabled;

    switch (GetMode()) {
        case 1: tickFlareon(lp); break;
        case 2: tickGlide(lp); break;
        case 3: tickFunny(lp); break;
        case 4: tickAirWalk(lp); break;
        case 0:
        default: tickCreative(lp); break;
    }
}

void Fly::OnPacketSend(SDK::Packet* packet, int packetId, bool& cancel) {
    if (!IsEnabled() || !packet) return;
    // Fly Blink: swallow movement packets (server-side freeze).
    if (GetFlyBlink() &&
        (packetId == (int)SDK::PacketID::PlayerAuthInput ||
         packetId == (int)SDK::PacketID::MovePlayer)) {
        cancel = true;
        return;
    }
    // Funny: pin the reported Y to the take-off height so the server never
    // sees the hover climb/sink.
    if (GetMode() == 3 && packetId == (int)SDK::PacketID::PlayerAuthInput) {
        if (!sdk_safe_ptr(packet, kPAI_mPos + sizeof(Vec3))) return;
        __try {
            Vec3* pos = reinterpret_cast<Vec3*>(reinterpret_cast<uintptr_t>(packet) + kPAI_mPos);
            pos->y = mStartY;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}
