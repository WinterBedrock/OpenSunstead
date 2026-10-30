#include "CameraClip.h"
#include "../../Memory/SigManager.h"
#include "../../Commands/CommandManager.h"
#include "../../Core/Logger.h"
#include "../../SDK/Types.h"
#include "MinHook.h"
#include <atomic>

namespace {
using TickFn = uintptr_t(__fastcall*)(void*, void**);
TickFn oCached = nullptr;
TickFn oNonCached = nullptr;
std::atomic<bool> g_enabled{false};
uintptr_t g_cachedVtable = 0;
uintptr_t g_nonCachedVtable = 0;
bool g_installed = false;

bool matchesVtable(void* self, uintptr_t expected) {
    __try {
        return self && expected && sdk_safe_ptr(self, sizeof(void*)) &&
               *reinterpret_cast<uintptr_t*>(self) == expected;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uintptr_t __fastcall hkCached(void* self, void** registryRef) {
    if (g_enabled.load(std::memory_order_relaxed) && matchesVtable(self, g_cachedVtable)) return 0;
    return oCached(self, registryRef);
}

uintptr_t __fastcall hkNonCached(void* self, void** registryRef) {
    if (g_enabled.load(std::memory_order_relaxed) && matchesVtable(self, g_nonCachedVtable)) return 0;
    return oNonCached(self, registryRef);
}

bool validPair(uintptr_t tick, uintptr_t table) {
    __try {
        return tick && table && sdk_safe_ptr(reinterpret_cast<void*>(tick), 16) &&
               sdk_safe_ptr(reinterpret_cast<void*>(table), 6 * sizeof(void*)) &&
               reinterpret_cast<uintptr_t*>(table)[5] == tick;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool installHooks() {
    if (g_installed) return true;
    auto& sig = SigManager::GetInstance();
    const uintptr_t cached = sig.CameraAvoidanceSystem_tickCached;
    const uintptr_t nonCached = sig.CameraAvoidanceSystem_tickNonCached;
    if (!validPair(cached, sig.CameraAvoidanceSystem_vtableCached) ||
        !validPair(nonCached, sig.CameraAvoidanceSystem_vtableNonCached)) return false;
    if (MH_CreateHook(reinterpret_cast<void*>(cached), &hkCached, reinterpret_cast<void**>(&oCached)) != MH_OK)
        return false;
    if (MH_CreateHook(reinterpret_cast<void*>(nonCached), &hkNonCached, reinterpret_cast<void**>(&oNonCached)) != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(cached));
        oCached = nullptr;
        return false;
    }
    g_cachedVtable = sig.CameraAvoidanceSystem_vtableCached;
    g_nonCachedVtable = sig.CameraAvoidanceSystem_vtableNonCached;
    if (MH_EnableHook(reinterpret_cast<void*>(cached)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(nonCached)) != MH_OK) {
        MH_DisableHook(reinterpret_cast<void*>(cached));
        MH_DisableHook(reinterpret_cast<void*>(nonCached));
        MH_RemoveHook(reinterpret_cast<void*>(cached));
        MH_RemoveHook(reinterpret_cast<void*>(nonCached));
        oCached = nullptr;
        oNonCached = nullptr;
        return false;
    }
    g_installed = true;
    return true;
}
}

void CameraClip::OnEnable() {
    if (!installHooks()) {
        CommandManager::GetInstance().PrintToChat("§cUnable to find CameraClipFunc!");
        LOG_ERROR("CameraClip: verified tick variants unavailable — module disabled", "CameraClip");
        SetEnabled(false);
        return;
    }
    g_enabled.store(true, std::memory_order_relaxed);
    LOG_INFO("CameraClip: both verified tick variants hooked", "CameraClip");
}

void CameraClip::OnDisable() { RestorePatch(); }

void CameraClip::RestorePatch() {
    g_enabled.store(false, std::memory_order_relaxed);
}
