#include "HookManager.h"
#include "DXHooks.h"
#include "GameHooks.h"

#include "../Memory/SigManager.h"
#include "MinHook.h"
#include "../Core/Logger.h"
#include <stdio.h>

void HookManager::Initialize() {
    LOG_INFO("Initializing HookManager", "HookManager");
    
    // Initialize MinHook globally
    if (MH_Initialize() != MH_OK) {
        LOG_ERROR("Failed to initialize MinHook", "HookManager");
        return;
    }

    DXHooks::Initialize();
    GameHooks::Initialize();
    isHooked = true;
}

void HookManager::Shutdown() {
    LOG_INFO("Shutting down HookManager", "HookManager");
    
    DXHooks::Shutdown();
    GameHooks::Shutdown();

    // Disable all hooks globally
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    isHooked = false;
}

uintptr_t HookManager::GetLocalPlayer() {
    // Removed old signatures as requested by user. LocalPlayer resolution disabled.
    return 0;
}
