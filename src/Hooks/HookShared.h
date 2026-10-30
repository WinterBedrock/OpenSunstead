#pragma once
// Internal glue shared by the GameHooks split TUs. Nothing here is exported
// outside src/Hooks — public surface stays in GameHooks.h.
#include <Windows.h>
#include <atomic>
#include "MinHook.h"

namespace gh {
    // MH_EnableHook suspends every thread in the process once per call, so
    // Initialize routes every enable through here: while a batch is open the
    // calls are only queued and one MH_EnableHook(MH_ALL_HOOKS) at the end
    // flips all trampolines in a single freeze.
    MH_STATUS HookEnable(LPVOID target);
    void BeginHookBatch();
    // Applies every queued enable in one thread freeze. Returns the queued
    // count (0 when nothing was queued) or -1 when the flush failed.
    int  FlushHookBatch();

    // Set once GameHooks::Initialize installed every detour. Hooks installed
    // before that (Present) must not run client paths mid-init.
    bool HooksInstalled();
    void SetHooksInstalled();

    bool isExecutablePtr(void* ptr);

    // ---- cross-group bridges (implemented in the owning TU) ----
    // NetworkHooks.cpp
    void tryHookPacketSend();
    // ChatHooks.cpp
    void runDeferredChatCommands();
    // GameModeHooks.cpp
    void tryResolveAndSwapPickRange();
    // NoFireHooks.cpp — lazy Actor::isOnFire vtable install
    void ensureNoFireIsOnFireHook();

    // ---- shared state (defined in the owning TU) ----
    // VisualHooks.cpp: last perspective the game asked for. Written by the
    // getPerspective hook, read by the visible-rotation override.
    extern std::atomic<int> g_lastPerspective;
    // BlockSourceHooks.cpp: live Dimension handed over by the
    // getTimeOfDay/getSkyColor hooks (owns the client BlockSource).
    extern void* g_liveDimension;
}

// InputHooks.cpp: cursor state for InventoryMove. Defined at global scope
// because InventoryMove.cpp externs it as a plain global.
extern bool g_IsCursorGrabbed;

// Per-group hook install entry points, called by GameHooks::Initialize in its
// documented order. Each lives in its namesake TU in src/Hooks.
// VisualHooks.cpp
void InstallGammaHook();
void InstallFreelookHooks();
void InstallWorldOverrideHooks();
// GameModeHooks.cpp
void InstallGameModeHooks();
// ItemPhysicsHooks.cpp
void InstallItemPhysicsHooks();
// WorldRenderHooks.cpp
void InstallRenderLevelHook();
void InstallNametagHook();
void InstallAttackHook();
void InstallSlowdownHook();
// ChatHooks.cpp
void InstallChatHooks();
// InputHooks.cpp
void InstallInputHooks();
// NetworkHooks.cpp
void InstallNetworkHooks();
// BlockSourceHooks.cpp
void InstallBlockSourceHooks();
// ScreenHooks.cpp
void InstallScreenHooks();
// CameraHooks.cpp
void InstallCameraHooks();
// NoFireHooks.cpp
void InstallNoFireHooks();
