#include "NoClip.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/SDK.h"
#include "../../SDK/Client/ScreenView.h"
#include <cmath>
#include <Windows.h>

// NoClip, v6 — see the header for why the spectator approach was dropped.
//
// Ported from the working reference implementation (Boost): collapse the AABB
// to zero height so the collision resolver stops ejecting us, then translate
// the box ourselves every tick from the look yaw + movement keys.
//
// Everything the game owns is written through SEH-guarded helpers. A bad
// pointer here degrades to "module does nothing", never to a crash — the same
// contract the rest of the SDK already holds.

static constexpr float kPi = 3.141592653589793f;
static constexpr float kVanillaHeight = 1.8f;

namespace {

// ---- SEH-guarded writers -------------------------------------------------
// Each touches one component the game also touches. A fault means the component
// was absent or the layout drifted, which is an expected outcome mid-world-load
// or during a dimension change — not a crash.

// Flatten the box: upper.y = lower.y. A zero-height box has no vertical extent,
// so the horizontal-collision pass finds nothing to resolve against.
bool sehCollapseHeight(SDK::Actor* lp, float* savedHeight) {
    if (!lp) return false;
    __try {
        auto* ab = lp->getAabbs();
        if (!ab || !sdk_safe_ptr(ab, sizeof(::AABBShapeComponent))) return false;
        if (savedHeight && ab->height > 0.5f && ab->height < 5.0f)
            *savedHeight = ab->height;
        ab->mMax.y = ab->mMin.y;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Restore the vanilla standing box.
bool sehRestoreHeight(SDK::Actor* lp, float height) {
    if (!lp) return false;
    __try {
        auto* ab = lp->getAabbs();
        if (!ab || !sdk_safe_ptr(ab, sizeof(::AABBShapeComponent))) return false;
        ab->mMax.y = ab->mMin.y + height;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Translate the live AABB by `delta` and apply the same delta to the
// StateVector, so the game's position/interpolation matches our write.
// Returns the delta actually applied (zero on failure), so the caller can
// chain the lerp.
bool sehApplyMove(SDK::Actor* lp, const Vec3& delta) {
    if (!lp) return false;
    __try {
        auto* ab = lp->getAabbs();
        if (!ab || !sdk_safe_ptr(ab, sizeof(::AABBShapeComponent))) return false;
        ab->mMin.x += delta.x; ab->mMin.y += delta.y; ab->mMin.z += delta.z;
        ab->mMax.x += delta.x; ab->mMax.y += delta.y; ab->mMax.z += delta.z;

        auto* sv = lp->getStateVector();
        if (sv && sdk_safe_ptr(sv, sizeof(StateVectorComponent))) {
            // posOld must follow: the renderer interpolates pos toward it, and
            // leaving it stale makes the camera snap/stutter on every write.
            sv->posOld.x = sv->pos.x; sv->posOld.y = sv->pos.y; sv->posOld.z = sv->pos.z;
            sv->pos.x += delta.x;     sv->pos.y += delta.y;     sv->pos.z += delta.z;
            sv->velocity = delta;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Pure hover: kill any residual horizontal drift and hold a small sink, so the
// player floats in place instead of being carried by leftover velocity.
bool sehHover(SDK::Actor* lp, float glide) {
    if (!lp) return false;
    __try {
        auto* sv = lp->getStateVector();
        if (!sv || !sdk_safe_ptr(sv, sizeof(StateVectorComponent))) return false;
        sv->velocity = Vec3{ 0.f, glide, 0.f };
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool sdkIsGameFocused() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD fgPid = 0;
    GetWindowThreadProcessId(fg, &fgPid);
    return fgPid == GetCurrentProcessId();
}

} // namespace

void NoClip::OnEnable() {
    mCollapsed = false;
    mSavedHeight = kVanillaHeight;

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci) return;
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp) return;

    mCollapsed = sehCollapseHeight(lp, &mSavedHeight);
}

void NoClip::OnDisable() {
    if (!mCollapsed) return;
    mCollapsed = false;

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci) return;
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp) return;

    sehRestoreHeight(lp, mSavedHeight);
    sehHover(lp, 0.f); // drop any velocity we were holding so landing is vanilla
}

void NoClip::OnTick() {
    if (!IsEnabled() || !mCollapsed) return;

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci) return;
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp) return;

    // Re-collapse every tick: the game rebuilds the box on damage, teleport or
    // a movement-tick, and once it is tall again the walls catch us.
    if (!sehCollapseHeight(lp, nullptr)) return;

    // Input inactive (chat open, window unfocused, menu): kill the residual
    // velocity instead of returning with it still set. The last tick's write
    // (e.g. +VSpeed while SPACE was held) otherwise keeps being integrated by
    // the game's motion system and the player slowly floats UP while typing -
    // the "glide pulls me into the air" effect.
    if (!sdkIsGameFocused() || !SDK::isGameplayInputActive()) {
        sehHover(lp, 0.f);
        return;
    }

    // ---- Build the world-space movement vector (Boost convention) --------
    // Forward is where the player LOOKS (yaw only — pitch would dive into the
    // floor). Strafe is 90 degrees off, same sign as the game's own input map.
    const bool w = (GetAsyncKeyState('W') & 0x8000) != 0;
    const bool a = (GetAsyncKeyState('A') & 0x8000) != 0;
    const bool s = (GetAsyncKeyState('S') & 0x8000) != 0;
    const bool d = (GetAsyncKeyState('D') & 0x8000) != 0;
    const bool up   = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
    const bool down = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

    const int strafe = (d ? 1 : 0) - (a ? 1 : 0);
    const int fwd    = (w ? 1 : 0) - (s ? 1 : 0);

    Vec3 delta{ 0.f, GetGlide(), 0.f };
    if (up)   delta.y += GetVSpeed();
    if (down) delta.y -= GetVSpeed();

    if (strafe != 0 || fwd != 0) {
        const float yaw = lp->getRot().y;
        const float angleDeg = std::atan2(static_cast<float>(strafe),
                                         static_cast<float>(fwd)) * (180.f / kPi);
        const float calcYaw = (yaw + angleDeg + 90.f) * (kPi / 180.f);
        delta.x = std::cos(calcYaw) * GetHSpeed();
        delta.z = std::sin(calcYaw) * GetHSpeed();
    }

    sehApplyMove(lp, delta);
}
