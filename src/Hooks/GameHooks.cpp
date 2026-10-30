// GameHooks::Initialize — the orchestrator. Installs every detour through the
// per-group Install*() entry points (each lives in its namesake TU in
// src/Hooks), then flushes the batch and publishes the readiness gate.
//
// Ordering rules this function preserves:
//  - every enable goes through gh::HookEnable, so one MH_EnableHook(MH_ALL_HOOKS)
//    at the end flips every trampoline in a single thread freeze;
//  - UI hooks go LAST (splash, ScreenView, camera, the NoFire set) — MinHook
//    freeszes threads mid-install and a live UI hook firing during that
//    window is what caused the old "half-dead inject" flakes;
//  - the readiness flag flips only after everything above, including
//    ColorCodeHook, is installed.
#include "GameHooks.h"
#include "HookShared.h"
#include "ColorCodeHook.h"
#include "../Memory/SigManager.h"
#include "../Commands/CommandManager.h"
#include "../Modules/ModuleManager.h"
#include "../Modules/Player/AnyOffhand.h"
#include "../Modules/Visual/Swing.h"
#include "../Modules/Visual/HotbarAnimation.h"
#include "../Modules/Visual/Chams.h"
#include "../SDK/Actor/MobEffects.h"
#include "../Core/Logger.h"

std::atomic<bool> GameHooks::g_freeCamGhostDrawn{ false };

// FreeCam ghost body: armed by the Chams ActorRenderDispatcher detour once it
// injected the local model, disarmed at the top of every world-render frame.
void GameHooks::ResetFreeCamGhostFrame() {
    g_freeCamGhostDrawn.store(false, std::memory_order_relaxed);
}

bool GameHooks::HooksReady() {
    return gh::HooksInstalled();
}

void GameHooks::Initialize() {
    LOG_INFO("Scanning signatures", "GameHooks");
    // Scans signatures, fills OffsetManager, publishes the g_* addresses.
    SigManager::GetInstance().Initialize();

    CommandManager::GetInstance().Initialize();

    // From here to FlushHookBatch every enable is queued and applied in ONE
    // pass at the end — each individual enable otherwise costs a full
    // process-thread freeze.
    gh::BeginHookBatch();

    InstallGammaHook();          // Fullbright
    InstallGameModeHooks();      // FastMine + PlaceDelay
    InstallFreelookHooks();      // getPerspective + _updatePlayer
    InstallItemPhysicsHooks();
    InstallWorldOverrideHooks(); // TimeChanger + FogColor

    InstallRenderLevelHook();
    InstallChatHooks();
    InstallInputHooks();
    InstallNetworkHooks();       // packet receive/send pipeline + ping capture

    // Module-owned hooks. Chams' ActorRenderDispatcher detour goes UP-FRONT:
    // it also carries FreeCam's ghost-body injection, which must run while
    // the Chams module itself is disabled; enabling Chams just re-arms the
    // device hooks through the same entry point.
    AnyOffhand::InstallHooks();
    Swing::InstallHooks();
    HotbarAnimation::InstallHooks();
    Chams::InstallHook();

    InstallNametagHook();
    InstallBlockSourceHooks();
    InstallAttackHook();
    InstallSlowdownHook();

    SDK::MobEffects::Initialize();

    // ---- UI hooks LAST (no concurrent render while MinHook is busy) -------
    InstallScreenHooks();         // splash, then ScreenView (absolute last)
    InstallCameraHooks();
    InstallNoFireHooks();

    // Custom §z colour code: own signature pair, installed after the rest so
    // a failure here cannot delay anything.
    ColorCodeHook::Initialize();

    gh::FlushHookBatch();

    gh::SetHooksInstalled();

    LOG_INFO("Game hooks initialized", "GameHooks");
}

void GameHooks::Shutdown() {
    // Restores the widened §-code letter gate. MinHook tears the detours down.
    ColorCodeHook::Shutdown();
}
