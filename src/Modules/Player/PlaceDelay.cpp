#include "PlaceDelay.h"
#include "../../SDK/Client/GameMode.h"
#include "../../SDK/Network/Packet.h"
#include "../../SDK/SDK.h"
#include "../ModuleManager.h"
#include "../Setting.h"
#include "../../Core/Logger.h"
#include <Windows.h>

// The flag lives in SDKRuntime.cpp next to the other published g_* globals.
// It is read by SDK::GameMode::buildBlock AND by the GameMode::buildBlock /
// continueDestroyBlock MinHooks (vanilla hold-to-place) — all of them zero the
// engine's placement stamps around the call while it is set.
extern bool SDK::GameMode::g_GameMode_SkipBuildCooldown;

// GameMode::mPlayer back-reference (gm+0x8) is the cheap "this is really a
// GameMode" test used across the SDK.
static bool looksLikeGameMode(void* gm, void* lp) {
    if (!gm || !lp || !sdk_safe_ptr(gm, ::Layout::GameMode::UseRepeatIntervalMs + 8))
        return false;
    __try {
        return *reinterpret_cast<void**>((uintptr_t)gm + ::Layout::GameMode::PlayerBackRef) == lp;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Zero BOTH stamps + force the repeat interval to 1 ms. POD-only body.
static bool sehForceFastRepeat(void* gm) {
    if (!gm || !sdk_safe_ptr(gm, ::Layout::GameMode::UseRepeatIntervalMs + 8)) return false;
    __try {
        const uintptr_t base = reinterpret_cast<uintptr_t>(gm);
        *reinterpret_cast<std::uint64_t*>(base + ::Layout::GameMode::BuildCooldownLastNs) = 0;
        *reinterpret_cast<std::uint64_t*>(base + ::Layout::GameMode::UseRepeatNextNs) = 0;
        // The engine's own hold-repeat gap: 250 ms by ctor default (reversed
        // from sub_142598DB0). 1 ms makes the engine's repeats run at tick
        // rate; the vanilla 250 is remembered so disable restores it.
        std::uint64_t& interval = *reinterpret_cast<std::uint64_t*>(base + ::Layout::GameMode::UseRepeatIntervalMs);
        if (interval > 1ull) interval = 1ull;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void restoreInterval(void* gm) {
    if (!gm || !sdk_safe_ptr(gm, ::Layout::GameMode::UseRepeatIntervalMs + 8)) return;
    __try {
        *reinterpret_cast<std::uint64_t*>((uintptr_t)gm + ::Layout::GameMode::UseRepeatIntervalMs) = 250ull;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

PlaceDelay::PlaceDelay()
    : Module("PlaceDelay", "Removes the client cooldown between block placements", Category::Player, 0) {
    AddSetting(new BoolSetting("Only While Scaffold", "Apply only while the Scaffold module is enabled", false)); // [0]
    AddSetting(new NumberSetting("Click Delay", "Delay between injected placements while holding RMB (ms)",
                                 50.0f, 0.0f, 500.0f, 5.0f));                                             // [1]
}

void PlaceDelay::OnEnable() {
    SDK::GameMode::g_GameMode_SkipBuildCooldown = true;
    mLastClickMs = 0;
    mPatchedGm = nullptr;
    LOG_INFO("PlaceDelay enabled - engine 250ms hold-repeat forced to 1ms + native clicks", "PlaceDelay");
}

void PlaceDelay::OnDisable() {
    SDK::GameMode::g_GameMode_SkipBuildCooldown = false;
    if (mPatchedGm) {
        restoreInterval(mPatchedGm);
        mPatchedGm = nullptr;
    }
    LOG_INFO("PlaceDelay disabled - vanilla 250ms hold-repeat restored", "PlaceDelay");
}

void PlaceDelay::OnTick() {
    bool want = true;
    if (GetOnlyScaffold()) {
        Module* sc = ModuleManager::GetInstance().GetModuleByName("Scaffold");
        want = sc && sc->IsEnabled();
    }
    if (SDK::GameMode::g_GameMode_SkipBuildCooldown != want)
        SDK::GameMode::g_GameMode_SkipBuildCooldown = want;

    if (!want) {
        if (mPatchedGm) { restoreInterval(mPatchedGm); mPatchedGm = nullptr; }
        return;
    }

    // Nothing to drive outside gameplay.
    if (!SDK::isGameplayInputActive()) return;
    const bool rmbHeld = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    if (!rmbHeld) {
        mLastClickMs = 0;   // the first click after a fresh press fires instantly
        return;
    }

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci) return;
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp) return;

    // Resolve the GameMode once per tick; remember it so OnDisable can undo
    // the interval patch even if the world is closing around us.
    void** vt = nullptr;
    void* gm = SDK::GameMode::resolve(lp, &vt);
    if (!gm || !vt || !looksLikeGameMode(gm, lp)) return;
    mPatchedGm = gm;

    // 1) Hold the engine's own gates open (stamps at epoch, interval at 1ms).
    sehForceFastRepeat(gm);

    // 2) Drive placement ourselves: inject a full game-native right-click at
    //    the module's rate. Each one runs the complete vanilla use path —
    //    the same transaction, prediction and packets a physical click
    //    produces — so the block lands exactly as vanilla, just without the
    //    engine pacing between them.
    float delayMs = GetClickDelayMs();
    if (delayMs < 0.0f) delayMs = 0.0f;
    const unsigned long long now = GetTickCount64();
    if (mLastClickMs == 0 || now - mLastClickMs >= (unsigned long long)delayMs) {
        SDK::requestRightClick();
        mLastClickMs = now;
    }
}
