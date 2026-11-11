#include "Reach.h"
#include "../../Memory/SigManager.h"
#include "../../Core/Logger.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {

struct LoadSite { size_t displacement; size_t instructionEnd; };
constexpr std::array<LoadSite, 2> kLoadSites{{{8, 12}, {18, 22}}};

uint8_t* capAnchor = nullptr;
std::atomic<float>* reachValue = nullptr;
std::array<int32_t, kLoadSites.size()> originalDisplacements{};
bool patched = false;

void* allocateNear(const uint8_t* anchor) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const uintptr_t granularity = info.dwAllocationGranularity;
    const uintptr_t base = reinterpret_cast<uintptr_t>(anchor);
    for (uintptr_t distance = granularity; distance < 0x70000000ull; distance += granularity) {
        const uintptr_t addresses[] = { base + distance, base - distance };
        for (const uintptr_t address : addresses) {
            void* page = VirtualAlloc(reinterpret_cast<void*>(address & ~(granularity - 1)),
                info.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (page) return page;
        }
    }
    return nullptr;
}

float loadCap(const uint8_t* instruction, const LoadSite& site) {
    int32_t displacement = 0;
    std::memcpy(&displacement, instruction + site.displacement, sizeof(displacement));
    float value = 0.f;
    std::memcpy(&value, instruction + site.instructionEnd + displacement, sizeof(value));
    return value;
}

bool validateSites(uintptr_t cap, uintptr_t clamp) {
    if (!cap || clamp <= cap || clamp - cap > 0x400) return false;
    __try {
        const auto* loads = reinterpret_cast<const uint8_t*>(cap);
        constexpr uint8_t prefix[] = { 0x84, 0xC0, 0x75 };
        constexpr uint8_t load[] = { 0xF3, 0x0F, 0x10, 0x3D };
        constexpr uint8_t clampBytes[] = {
            0x84, 0xC0, 0x74, 0x07, 0xF3, 0x44, 0x0F, 0x5D, 0xC7
        };
        if (std::memcmp(loads, prefix, sizeof(prefix)) != 0 ||
            std::memcmp(loads + 4, load, sizeof(load)) != 0 ||
            std::memcmp(loads + 14, load, sizeof(load)) != 0 ||
            std::memcmp(reinterpret_cast<const void*>(clamp), clampBytes, sizeof(clampBytes)) != 0)
            return false;
        return loadCap(loads, kLoadSites[0]) == 3.f && loadCap(loads, kLoadSites[1]) == 7.f;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool writeDisplacement(size_t index, int32_t value) {
    uint8_t* destination = capAnchor + kLoadSites[index].displacement;
    DWORD oldProtection = 0;
    if (!VirtualProtect(destination, sizeof(value), PAGE_EXECUTE_READWRITE, &oldProtection)) return false;
    std::atomic_ref(*reinterpret_cast<int32_t*>(destination)).store(value, std::memory_order_release);
    FlushInstructionCache(GetCurrentProcess(), destination, sizeof(value));
    DWORD ignored = 0;
    return VirtualProtect(destination, sizeof(value), oldProtection, &ignored) != FALSE;
}

bool restore() {
    bool restored = true;
    for (size_t i = 0; i < originalDisplacements.size(); ++i)
        restored = writeDisplacement(i, originalDisplacements[i]) && restored;
    if (restored) patched = false;
    return restored;
}

}

void Reach::OnEnable() {
    if (patched) {
        LOG_ERROR("Reach cannot re-enable over an existing code patch", "Reach");
        SetEnabled(false);
        return;
    }

    auto& sigs = SigManager::GetInstance();
    const uintptr_t anchor = sigs.ReachClampCapLoadAnchor12660;
    if (!validateSites(anchor, sigs.ReachClampRegisterAnchor12660)) {
        LOG_WARN("Reach range constants did not match the verified game path", "Reach");
        SetEnabled(false);
        return;
    }

    auto* page = static_cast<std::atomic<float>*>(allocateNear(reinterpret_cast<uint8_t*>(anchor)));
    if (!page) {
        LOG_ERROR("Reach could not allocate nearby storage", "Reach");
        SetEnabled(false);
        return;
    }
    capAnchor = reinterpret_cast<uint8_t*>(anchor);
    reachValue = page;
    reachValue->store(GetReachDistance(), std::memory_order_release);

    std::array<int32_t, kLoadSites.size()> replacements{};
    for (size_t i = 0; i < replacements.size(); ++i) {
        const intptr_t delta = reinterpret_cast<intptr_t>(reachValue) -
            reinterpret_cast<intptr_t>(capAnchor + kLoadSites[i].instructionEnd);
        if (delta < std::numeric_limits<int32_t>::min() || delta > std::numeric_limits<int32_t>::max()) {
            SetEnabled(false);
            return;
        }
        replacements[i] = static_cast<int32_t>(delta);
        std::memcpy(&originalDisplacements[i], capAnchor + kLoadSites[i].displacement, sizeof(int32_t));
    }

    size_t written = 0;
    for (; written < replacements.size(); ++written) {
        if (!writeDisplacement(written, replacements[written])) break;
        patched = true;
    }
    if (written != replacements.size()) {
        if (patched && !restore()) LOG_ERROR("Reach patch rollback failed", "Reach");
        SetEnabled(false);
    }
}

void Reach::OnDisable() {
    if (patched && !restore())
        LOG_ERROR("Reach original range constants could not be restored", "Reach");
}

void Reach::OnRender() {
    if (patched && reachValue)
        reachValue->store(GetReachDistance(), std::memory_order_release);
}

float Reach::GetReachDistance() const {
    return static_cast<NumberSetting*>(mSettings[0])->mValue;
}
