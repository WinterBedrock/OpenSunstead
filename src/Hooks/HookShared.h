#pragma once
#include <Windows.h>
#include <atomic>

namespace gh {
    bool HooksInstalled();
    void SetHooksInstalled();

    bool isExecutablePtr(void* ptr);

    void tryHookPacketSend();
    void runDeferredChatCommands();

    void ensureNoFireIsOnFireHook();

    extern std::atomic<int> g_lastPerspective;
    extern void* g_liveDimension;
}

extern bool g_IsCursorGrabbed;

void InstallGammaHook();
void InstallFreelookHooks();
void InstallWorldOverrideHooks();
void InstallGameModeHooks();
void InstallItemPhysicsHooks();
void InstallRenderLevelHook();
void InstallNametagHook();
void InstallAttackHook();
void InstallSlowdownHook();
void InstallChatHooks();
void InstallInputHooks();
void InstallNetworkHooks();
void InstallBlockSourceHooks();
void InstallScreenHooks();
void InstallCameraHooks();
void InstallNoFireHooks();
