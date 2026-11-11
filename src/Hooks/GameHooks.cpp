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

void GameHooks::ResetFreeCamGhostFrame() {
    g_freeCamGhostDrawn.store(false, std::memory_order_relaxed);
}

bool GameHooks::HooksReady() {
    return gh::HooksInstalled();
}

void GameHooks::Initialize() {
    LOG_INFO("Scanning signatures", "GameHooks");
    SigManager::GetInstance().Initialize();

    CommandManager::GetInstance().Initialize();

    InstallGammaHook();
    InstallGameModeHooks();
    InstallFreelookHooks();
    InstallItemPhysicsHooks();
    InstallWorldOverrideHooks();

    InstallRenderLevelHook();
    InstallChatHooks();
    InstallInputHooks();
    InstallNetworkHooks();

    AnyOffhand::InstallHooks();
    Swing::InstallHooks();
    HotbarAnimation::InstallHooks();
    Chams::InstallHook();

    InstallNametagHook();
    InstallBlockSourceHooks();
    InstallAttackHook();
    InstallSlowdownHook();

    SDK::MobEffects::Initialize();

    InstallScreenHooks();
    InstallCameraHooks();
    InstallNoFireHooks();

    ColorCodeHook::Initialize();

    gh::SetHooksInstalled();

    LOG_INFO("Game hooks initialized", "GameHooks");
}

void GameHooks::Shutdown() {
    ColorCodeHook::Shutdown();
}
