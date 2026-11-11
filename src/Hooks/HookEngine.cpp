#include "HookEngine.h"
#include "../Core/Logger.h"

#include <safetyhook.hpp>

#include <map>
#include <mutex>
#include <string>

namespace {

struct HookEntry {
    safetyhook::InlineHook hook;
    void* original = nullptr;   // raw trampoline pointer
};

std::mutex g_mutex;
// Keyed by target function address.
std::map<void*, HookEntry> g_hooks;

const char* errorName(const safetyhook::InlineHook::Error& e) {
    switch (e.type) {
    case safetyhook::InlineHook::Error::BAD_ALLOCATION: return "BAD_ALLOCATION";
    case safetyhook::InlineHook::Error::FAILED_TO_DECODE_INSTRUCTION: return "FAILED_TO_DECODE_INSTRUCTION";
    case safetyhook::InlineHook::Error::SHORT_JUMP_IN_TRAMPOLINE: return "SHORT_JUMP_IN_TRAMPOLINE";
    case safetyhook::InlineHook::Error::IP_RELATIVE_INSTRUCTION_OUT_OF_RANGE: return "IP_RELATIVE_INSTRUCTION_OUT_OF_RANGE";
    case safetyhook::InlineHook::Error::UNSUPPORTED_INSTRUCTION_IN_TRAMPOLINE: return "UNSUPPORTED_INSTRUCTION_IN_TRAMPOLINE";
    case safetyhook::InlineHook::Error::FAILED_TO_UNPROTECT: return "FAILED_TO_UNPROTECT";
    case safetyhook::InlineHook::Error::NOT_ENOUGH_SPACE: return "NOT_ENOUGH_SPACE";
    default: return "UNKNOWN";
    }
}

} // namespace

namespace HookEngine {

bool Create(void* target, void* detour, void** original) {
    return Create(target, detour, original, true);
}

bool Create(void* target, void* detour, void** original, bool startEnabled) {
    if (!target || !detour) return false;

    std::lock_guard lock(g_mutex);
    auto it = g_hooks.find(target);
    if (it != g_hooks.end()) {
        // already hooked: just hand back the existing trampoline and re enable
        if (original && !*original) *original = it->second.original;
        return Enable(target);
    }

    const auto flags = startEnabled ? safetyhook::InlineHook::Default
                                     : safetyhook::InlineHook::StartDisabled;
    auto result = safetyhook::InlineHook::create(target, detour, flags);
    if (!result) {
        char buf[260];
        std::snprintf(buf, sizeof(buf), "SafetyHook create failed @ %p: %s",
                      target, errorName(result.error()));
        LOG_ERROR(buf, "HookEngine");
        return false;
    }

    HookEntry entry;
    entry.hook = std::move(result).value();
    entry.original = entry.hook.original<void (*)()>();
    const bool ok = static_cast<bool>(entry.hook);
    void* const tramp = entry.original;
    g_hooks.emplace(target, std::move(entry));
    if (original) *original = tramp;
    return ok;
}

bool IsHooked(void* target) {
    std::lock_guard lock(g_mutex);
    return target && g_hooks.count(target) != 0;
}

bool Enable(void* target) {
    std::lock_guard lock(g_mutex);
    auto it = g_hooks.find(target);
    if (it == g_hooks.end()) return false;
    if (it->second.hook.enabled()) return true;
    auto result = it->second.hook.enable();
    if (!result) {
        char buf[200];
        std::snprintf(buf, sizeof(buf), "SafetyHook enable failed @ %p: %s",
                      target, errorName(result.error()));
        LOG_ERROR(buf, "HookEngine");
        return false;
    }
    return true;
}

bool Disable(void* target) {
    std::lock_guard lock(g_mutex);
    auto it = g_hooks.find(target);
    if (it == g_hooks.end()) return false;
    if (!it->second.hook.enabled()) return true;
    auto result = it->second.hook.disable();
    if (!result) {
        char buf[200];
        std::snprintf(buf, sizeof(buf), "SafetyHook disable failed @ %p: %s",
                      target, errorName(result.error()));
        LOG_ERROR(buf, "HookEngine");
        return false;
    }
    return true;
}

void Remove(void* target) {
    std::lock_guard lock(g_mutex);
    g_hooks.erase(target);   // InlineHook::reset runs from the destructor
}

void ShutdownAll() {
    std::lock_guard lock(g_mutex);
    LOG_INFO("Unhooking " + std::to_string(g_hooks.size()) +
             " hooks (SafetyHook restore)", "HookEngine");
    g_hooks.clear();
}

} // namespace HookEngine
