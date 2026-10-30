#include "Reach.h"
#include "../../Memory/SigManager.h"
#include "../Setting.h"
#include "../../Core/Logger.h"
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {
struct Site { int dispOff; int endOff; };
constexpr Site kSites[] = {{8, 12}, {18, 22}};
uint8_t* g_anchor = nullptr;
float* g_reachVal = nullptr;
int32_t g_origDisp[2]{};
bool g_patched = false;

void* AllocNear(uint8_t* anchorPtr) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t base = reinterpret_cast<uintptr_t>(anchorPtr);
    for (uintptr_t off = gran; off < 0x70000000ull; off += gran) {
        const uintptr_t candidates[] = {base + off, base - off};
        for (uintptr_t candidate : candidates) {
            void* p = VirtualAlloc(reinterpret_cast<void*>(candidate & ~(gran - 1)), si.dwPageSize,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (p) return p;
        }
    }
    return nullptr;
}

bool validateAnchors(uintptr_t capAnchor, uintptr_t registerAnchor) {
    if (!capAnchor || registerAnchor <= capAnchor || registerAnchor - capAnchor >= 0x200) return false;
    __try {
        const auto* p = reinterpret_cast<const uint8_t*>(capAnchor);
        const auto* clamp = reinterpret_cast<const uint8_t*>(registerAnchor);
        const uint8_t load[] = {0xF3, 0x0F, 0x10, 0x3D};
        const uint8_t minss[] = {0xF3, 0x44, 0x0F, 0x5D, 0xC7};
        if (p[0] != 0x84 || p[1] != 0xC0 || p[2] != 0x75 || p[3] != 0x0A ||
            p[12] != 0xEB || p[13] != 0x08 ||
            std::memcmp(p + 4, load, sizeof(load)) != 0 ||
            std::memcmp(p + 14, load, sizeof(load)) != 0 ||
            clamp[0] != 0x84 || clamp[1] != 0xC0 || clamp[2] != 0x74 ||
            std::memcmp(clamp + 4, minss, sizeof(minss)) != 0) return false;
        const float expected[] = {3.0f, 7.0f};
        for (size_t i = 0; i < 2; ++i) {
            int32_t displacement;
            std::memcpy(&displacement, p + kSites[i].dispOff, sizeof(displacement));
            if (*reinterpret_cast<const float*>(p + kSites[i].endOff + displacement) != expected[i]) return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool writeDisplacement(size_t index, int32_t value) {
    uint8_t* destination = g_anchor + kSites[index].dispOff;
    DWORD oldProtection;
    if (!VirtualProtect(destination, sizeof(value), PAGE_EXECUTE_READWRITE, &oldProtection)) return false;
    std::memcpy(destination, &value, sizeof(value));
    DWORD ignored;
    VirtualProtect(destination, sizeof(value), oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), destination, sizeof(value));
    return true;
}

bool restoreOperands() {
    bool ok = true;
    for (size_t i = 0; i < 2; ++i)
        if (!writeDisplacement(i, g_origDisp[i])) ok = false;
    if (ok) g_patched = false;
    return ok;
}
}

void Reach::OnEnable() {
    if (g_patched && !restoreOperands()) {
        LOG_ERROR("Reach remains partially patched; enable aborted", "Reach");
        SetEnabled(false);
        return;
    }
    auto& sig = SigManager::GetInstance();
    const uintptr_t anchor = sig.ReachClampCapLoadAnchor12660;
    if (!validateAnchors(anchor, sig.ReachClampRegisterAnchor12660)) {
        LOG_WARN("Verified reach cap-load sites unavailable; disabling", "Reach");
        SetEnabled(false);
        return;
    }
    g_anchor = reinterpret_cast<uint8_t*>(anchor);
    if (!g_reachVal) g_reachVal = reinterpret_cast<float*>(AllocNear(g_anchor));
    if (!g_reachVal) {
        LOG_ERROR("AllocNear failed; disabling", "Reach");
        SetEnabled(false);
        return;
    }
    *g_reachVal = GetReachDistance();
    int32_t replacements[2];
    for (size_t i = 0; i < 2; ++i) {
        const intptr_t delta = reinterpret_cast<intptr_t>(g_reachVal) -
                               reinterpret_cast<intptr_t>(g_anchor + kSites[i].endOff);
        if (delta < std::numeric_limits<int32_t>::min() || delta > std::numeric_limits<int32_t>::max()) {
            LOG_ERROR("Reach value outside RIP displacement range; disabling", "Reach");
            SetEnabled(false);
            return;
        }
        replacements[i] = static_cast<int32_t>(delta);
        std::memcpy(&g_origDisp[i], g_anchor + kSites[i].dispOff, sizeof(int32_t));
    }
    size_t written = 0;
    for (; written < 2; ++written)
        if (!writeDisplacement(written, replacements[written])) break;
    if (written != 2) {
        g_patched = written != 0;
        if (g_patched && !restoreOperands()) LOG_ERROR("Reach rollback failed", "Reach");
        LOG_ERROR("Reach patch transaction failed; disabling", "Reach");
        SetEnabled(false);
        return;
    }
    g_patched = true;
    LOG_INFO("Reach enabled with verified two-site cap loads", "Reach");
}

void Reach::OnDisable() {
    if (g_patched && !restoreOperands()) {
        LOG_ERROR("Reach operands could not be fully restored", "Reach");
        return;
    }
    LOG_INFO("Reach disabled; original caps restored", "Reach");
}

void Reach::OnRender() {
    if (IsEnabled() && g_patched && g_reachVal) *g_reachVal = GetReachDistance();
}

float Reach::GetReachDistance() const {
    return static_cast<NumberSetting*>(mSettings[0])->mValue;
}
