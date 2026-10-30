#include "AimAssist.h"
#include "../../Utils/MCSDK.h"
#include "../ModuleManager.h"
#include "../Misc/AntiBot.h"
#include <vector>
#include <cmath>
#include <Windows.h>

static bool calcAim(const Vec3& eye, const Vec3& aim, float* pitchOut, float* yawOut) {
    if (vecHorizontalDistance(aim, eye) < 1e-4f) return false;
    const Vec2 look = calcLookAngles(eye, aim);
    *pitchOut = look.x;
    *yawOut   = look.y;
    return true;
}


static bool sehApplySmoothAim(SDK::Actor* lp, float targetPitch, float targetYaw,
                              float smooth, float dt) {
    __try {
        ActorRotationComponent* rc = lp->getRotation();
        if (!rc) return false;
        float curPitch = rc->rotation.x;
        float curYaw = rc->rotation.y;
        Vec2 delta = { curPitch - targetPitch, angleDelta(curYaw, targetYaw) };
        if (smooth > 0.1f) { delta.x /= smooth; delta.y /= smooth; }
        delta.x *= dt;
        delta.y *= dt;
        if (std::fabs(delta.x) < 1e-6f && std::fabs(delta.y) < 1e-6f) return false;
        lp->applyTurnDelta(delta);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void AimAssist::OnDisable() {
    mLastAimMs = 0;
}

void AimAssist::OnUpdate() {
    if (!IsEnabled()) return;

    SDK::Level* level = nullptr;
    SDK::Actor* lp = nullptr;
    if (!SDK::resolveContext(nullptr, &level, &lp)) return;

    unsigned long long now = GetTickCount64();
    float dt = 1.0f / 60.0f;
    if (mLastAimMs != 0) {
        dt = (float)(now - mLastAimMs) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;
        if (dt < 1e-4f) return;
    }
    mLastAimMs = now;

    Vec3 eye{};
    if (!sehGetPos(lp, &eye)) return; // getPos == eye height for local player

    ActorRotationComponent* rc = lp->getRotation();
    if (!rc) return;
    float curPitch = rc->rotation.x;
    float curYaw = rc->rotation.y;

    std::vector<SDK::Actor*> actors;
    if (!sehGetActorList(level, &actors)) return;

    float range = GetRange();
    float range2 = range * range;
    float maxFov = GetFOV();
    int aimPoint = GetAimPoint();

    AntiBot* antiBot = static_cast<AntiBot*>(ModuleManager::GetInstance().GetModuleByName("AntiBot"));

    SDK::Actor* best = nullptr;
    float bestT2 = 1e30f;
    float bestPitch = 0, bestYaw = 0;

    for (SDK::Actor* a : actors) {
        if (!a || a == lp) continue;
        if (!sdk_safe_ptr(a, 0x230)) continue;
        if (!sehIsPlayer(a)) continue;
        if (antiBot && antiBot->IsBot(a)) continue;

        Vec3 eyePos{};
        if (!sehGetPos(a, &eyePos)) continue;
        float dx = eyePos.x - eye.x, dy = eyePos.y - eye.y, dz = eyePos.z - eye.z;
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > range2) continue;

        Vec3 bmn, bmx, aim = eyePos;
        if (sehGetActorBox(a, &bmn, &bmx))
            aim = SDK::aimPointOnBox(bmn, bmx, aimPoint);

        float tp = 0, ty = 0;
        if (!calcAim(eye, aim, &tp, &ty)) continue;

        float dPitch = std::fabs(tp - curPitch);
        float dYaw = std::fabs(angleDelta(curYaw, ty));
        if (dPitch > maxFov || dYaw > maxFov) continue;

        if (d2 < bestT2) {
            bestT2 = d2;
            best = a;
            bestPitch = tp;
            bestYaw = ty;
        }
    }

    if (!best) return;
    sehApplySmoothAim(lp, bestPitch, bestYaw, GetSmooth(), dt);
}