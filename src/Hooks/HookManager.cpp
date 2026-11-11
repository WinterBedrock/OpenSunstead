#include "HookManager.h"
#include "DXHooks.h"
#include "GameHooks.h"
#include "HookEngine.h"

#include "../Core/Logger.h"
#include <stdio.h>

void HookManager::Initialize() {
    LOG_INFO("Initializing HookManager (SafetyHook)", "HookManager");

    DXHooks::Initialize();
    GameHooks::Initialize();
    isHooked = true;
}

void HookManager::Shutdown() {
    LOG_INFO("Shutting down HookManager", "HookManager");

    DXHooks::Shutdown();
    GameHooks::Shutdown();

    // disable and destroy every registered hook
    HookEngine::ShutdownAll();

    isHooked = false;
}
