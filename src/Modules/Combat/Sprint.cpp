#include "Sprint.h"
#include "../Player/GhostHand.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/SDK.h"
#include "../../SDK/Client/LevelRendererPlayer.h"
#include "../../Memory/SigManager.h"
#include "../../Core/Logger.h"
#include "../../Render/FrameView.h"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Utils/MiscUtils/MathUtils.hpp"
#include "MinHook.h"
#include <Windows.h>
#include <atomic>
#include <cmath>

// Sprint — hold sprint ON while the player walks forward.
//
// The input system rebuilds MoveInputComponent from the real keyboard on every
// ClientInputUpdateSystem::tick (Latite hooks the same function and writes
// AFTER it). Writing from OnTick races that rebuild: the bit sticks on the
// ticks where we win and vanishes on the ones where the rebuild wins, which is
// the "sprints, then doesn't" flicker.
//
// The bit itself is the one Baritone already writes and the game's own
// press/release lambdas index:
//
//     *(uint32_t*)(component + 4*(flag>>5) + 0x10) |= 1u << flag
//
// Flag 8 is sprint (Baritone::FlagSprint, "button.sprint"), so it is bit 8 of
// the single dword at +0x10 — the second byte, mask 0x01. Latite's
// rawInputState.sprintDown is that same bit. The earlier byte writes at +0x01
// and +0x11 were the first two bytes of that dword's NEIGHBOURS, not the flag.

// rawInputState.sprintDown: flag dword +0x10, bit 8.
static constexpr uintptr_t kFlagDword   = 0x10;
static constexpr uint32_t  kSprintBit   = 1u << 8;
static constexpr size_t    kMoveInputSize = 0x64;

using MoveInputGet = void* (__fastcall*)(void* ctx);

static void* componentOf(SDK::Actor* lp) {
    if (!lp || !g_MoveInputComponent_get) return nullptr;
    if (!sdk_safe_ptr((void*)g_MoveInputComponent_get, 16)) return nullptr;
    auto* ctx = (SDK::EntityContext*)((uintptr_t)lp + 0x8);
    if (!sdk_safe_ptr(ctx, 0x18)) return nullptr;
    void* p = ((MoveInputGet)g_MoveInputComponent_get)(ctx);
    if (!p || !sdk_safe_ptr(p, kMoveInputSize)) return nullptr;
    return p;
}

// `on` sets the bit, `off` clears only our bit — the rest of the dword is the
// player's real keys and must survive.
static bool sehWriteSprintBit(SDK::Actor* lp, bool on) {
    if (!lp) return false;
    __try {
        void* mi = componentOf(lp);
        if (!mi) return false;
        auto* flags = (uint32_t*)((uintptr_t)mi + kFlagDword);
        if (on) *flags |= kSprintBit;
        else    *flags &= ~kSprintBit;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool sehAssertSprint(SDK::Actor* lp) {
    if (!lp) return false;
    __try {
        return lp->setStatusFlag(ActorFlags::Sprinting, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- post-input hook --------------------------------------------------------
// Latite's signature, verified unique (1 match) on 1.26.51.01. The function
// rebuilds the component from the keyboard and returns; we write afterwards,
// so our bit is the last word until the next tick.

using InputTickFn = void(__fastcall*)(
    void*, void*, void*, void*, void*, void*, void*, void*,
    void*, void*, void*, void*, void*, void*, void*, void*, void*, void*);

static InputTickFn oInputTick = nullptr;
static std::atomic<bool> g_wantSprint{ false };
static bool s_hookInstalled = false;

static void __fastcall hkInputTick(
    void* a1, void* a2, void* a3, void* a4, void* a5, void* a6,
    void* a7, void* a8, void* a9, void* a10, void* a11, void* a12,
    void* a13, void* a14, void* a15, void* a16, void* a17, void* a18) {
    if (oInputTick)
        oInputTick(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10,
                   a11, a12, a13, a14, a15, a16, a17, a18);
    Sprint::AfterInputTick();
    GhostHand::AfterInputTick();
}

void Sprint::EnsureInputHook() {
    if (s_hookInstalled) return;
    const uintptr_t fn = SigManager::GetInstance().ClientInput_tick;
    if (!fn) {
        LOG_WARN("ClientInput tick not found — sprint falls back to OnTick writes", "Sprint");
        return;
    }
    if (MH_CreateHook(reinterpret_cast<LPVOID>(fn), reinterpret_cast<LPVOID>(&hkInputTick),
                      reinterpret_cast<LPVOID*>(&oInputTick)) != MH_OK
        || MH_EnableHook(reinterpret_cast<LPVOID>(fn)) != MH_OK) {
        LOG_ERROR("ClientInput tick hook failed", "Sprint");
        return;
    }
    s_hookInstalled = true;
    LOG_INFO("ClientInput tick hooked (sprint writes after the input rebuild)", "Sprint");
}

bool Sprint::HookInstalled() { return s_hookInstalled; }

void Sprint::AfterInputTick() {
    if (!g_wantSprint.load(std::memory_order_relaxed)) return;
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci) return;
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp) return;
    sehWriteSprintBit(lp, true);
    sehAssertSprint(lp);
}

void Sprint::applySprintInput(SDK::Actor* lp) {
    // W/SHIFT are text input while the chat (or any other screen) is open —
    // never read them there, and stop asserting sprint until the HUD returns.
    if (!SDK::isGameplayInputActive()) {
        g_wantSprint.store(false, std::memory_order_relaxed);
        return;
    }

    const bool forward = (GetAsyncKeyState('W') & 0x8000) != 0;
    const bool sneak   = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool want    = forward && !sneak;

    // The hook is the authoritative write. This one covers the window before
    // the hook is installed, and the case where the input system is not ticking.
    g_wantSprint.store(want, std::memory_order_relaxed);
    sehWriteSprintBit(lp, want);
    if (want) sehAssertSprint(lp);
}

// ---- FOV effect -------------------------------------------------------------
// The kick is applied by the shared SDK::sehApplyFovMod writer (renderLevel
// hook, before the world render) from the PUBLISHED multiplier below. This
// module only computes and publishes it — writing the LevelRendererPlayer
// projection elements from here with a frozen base stomped the game's own FOV
// animation and made world-to-screen overlays slide while moving (see
// LevelRendererPlayer.h).
static float s_fovMod = 1.f;                        // smoothed multiplier

// Definition of the overlay sprint-FOV multiplier (declared in FrameView.h).
// 1.0 = no kick. Written every applyFovEffect; read by the FrameTransform
// producer (recorded per push) and acquireCameraMVP.
volatile float g_sprintFovScale = 1.0f;

void Sprint::applyFovEffect() {
    const bool effectOn = !mSettings.empty() && static_cast<BoolSetting*>(mSettings[0])->mValue;
    const float amount = mSettings.size() > 1 ? static_cast<NumberSetting*>(mSettings[1])->mValue : 0.92f;

    const bool forward = SDK::isGameplayInputActive() &&
                         (GetAsyncKeyState('W') & 0x8000) != 0 &&
                         (GetAsyncKeyState(VK_SHIFT) & 0x8000) == 0;
    // Zoom owns the FOV field while it magnifies — hand it back without a kick.
    const bool zooming = g_zoomProjScale > 1.001f || g_zoomProjScale < 0.999f;
    if (zooming) {
        s_fovMod = 1.f;
        g_sprintFovScale = 1.0f;   // overlay: sprint kick off while zooming
        return;
    }
    const float target = (effectOn && forward) ? amount : 1.0f;

    if (target == 1.0f && s_fovMod == 1.0f) {
        g_sprintFovScale = 1.0f;
        return;   // never touched / fully released
    }

    const float dt = ImRenderUtils::getDeltaTime();
    s_fovMod = MathUtils::lerp(s_fovMod, target, dt * 8.0f);
    if (std::abs(s_fovMod - target) < 0.005f) s_fovMod = target;
    // Publish for the world render (sehApplyFovMod scales the LRP projection
    // focal elements by zoom * sprint before the draw) and for the overlay
    // projection (FrameView records the same value per push).
    g_sprintFovScale = s_fovMod;
}

void Sprint::OnTick() {
    if (!IsEnabled()) return;

    SDK::ClientInstance* cInst = SDK::ClientInstance::get();
    if (!cInst) return;
    SDK::Actor* lp = cInst->getLocalPlayer();
    if (!lp) return;

    applySprintInput(lp);
    applyFovEffect();
}

void Sprint::OnUpdate() {
    if (!IsEnabled()) return;

    SDK::ClientInstance* cInst = SDK::ClientInstance::get();
    if (!cInst) return;
    SDK::Actor* lp = cInst->getLocalPlayer();
    if (!lp) return;

    applySprintInput(lp);
}

void Sprint::OnEnable() {
    EnsureInputHook();
}

void Sprint::OnDisable() {
    g_wantSprint.store(false, std::memory_order_relaxed);
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (ci) {
        SDK::Actor* lp = ci->getLocalPlayer();
        if (lp) sehWriteSprintBit(lp, false);
    }
    // Give the game's FOV back if the effect was mid-flight: just drop the
    // published multiplier to 1 — the shared SDK::sehApplyFovMod (renderLevel
    // hook) divides out the scale it applied last frame on its next write, so
    // the game's live animated base is restored with no manual pointer writes.
    s_fovMod = 1.f;
    g_sprintFovScale = 1.0f;
}
