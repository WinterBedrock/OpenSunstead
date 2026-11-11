#include "CameraClip.h"
#include "../../Memory/SigManager.h"
#include "../../Commands/CommandManager.h"
#include "../../Core/Logger.h"
#include "../../SDK/Types.h"
#include "../../Hooks/HookEngine.h"
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
    // Create both jumps disabled so a failure half-way never leaves an active
    // hook without its bookkeeping.
    if (!HookEngine::Create(reinterpret_cast<void*>(cached), &hkCached,
                            reinterpret_cast<void**>(&oCached), false))
        return false;
    if (!HookEngine::Create(reinterpret_cast<void*>(nonCached), &hkNonCached,
                            reinterpret_cast<void**>(&oNonCached), false)) {
        HookEngine::Remove(reinterpret_cast<void*>(cached));
        oCached = nullptr;
        return false;
    }
    g_cachedVtable = sig.CameraAvoidanceSystem_vtableCached;
    g_nonCachedVtable = sig.CameraAvoidanceSystem_vtableNonCached;
    if (!HookEngine::Enable(reinterpret_cast<void*>(cached)) ||
        !HookEngine::Enable(reinterpret_cast<void*>(nonCached))) {
        HookEngine::Remove(reinterpret_cast<void*>(cached));
        HookEngine::Remove(reinterpret_cast<void*>(nonCached));
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
        //CommandManager::GetInstance().PrintToChat("§cUnable to find CameraClipFunc!");
        //LOG_ERROR("CameraClip: verified tick variants unavailable — module disabled", "CameraClip");
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
