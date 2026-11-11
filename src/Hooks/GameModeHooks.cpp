#include "GameHooks.h"
#include "HookShared.h"
#include "../SDK/SDK.h"
#include "../SDK/Client/GameMode.h"
#include "../Modules/ModuleManager.h"
#include "../Modules/Player/FastMine.h"
#include "../Core/Logger.h"
#include "../Memory/SigManager.h"
#include "HookEngine.h"
#include <algorithm>

namespace {
    using getDestroyRate_t = float(__fastcall*)(void*, void*);
    getDestroyRate_t oGetDestroyRate = nullptr;

    float __fastcall hkGetDestroyRate(void* gm, void* block) {
        SDK::GameMode::remember(gm);
        const float rate = oGetDestroyRate ? oGetDestroyRate(gm, block) : 0.f;
        auto* fastMine = ModuleManager::GetInstance().GetModuleByName("FastMine");
        if (fastMine && fastMine->IsEnabled())
            return rate * std::clamp(static_cast<FastMine*>(fastMine)->GetMultiplier(), 0.1f, 50.f);
        return rate;
    }

    void zeroCooldowns(void* gm) {
        if (!gm || !sdk_safe_ptr(gm, ::Layout::GameMode::UseRepeatIntervalMs + 8)) return;
        __try {
            const auto base = reinterpret_cast<uintptr_t>(gm);
            *reinterpret_cast<uint64_t*>(base + ::Layout::GameMode::BuildCooldownLastNs) = 0;
            *reinterpret_cast<uint64_t*>(base + ::Layout::GameMode::UseRepeatNextNs) = 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    using buildBlock_t = void(__fastcall*)(void*, const SDK::IVec3*, unsigned char, bool, float);
    buildBlock_t oBuildBlock = nullptr;

    void __fastcall hkBuildBlock(void* gm, const SDK::IVec3* pos, unsigned char face, bool itemSource, float scale) {
        SDK::GameMode::remember(gm);
        if (SDK::GameMode::g_GameMode_SkipBuildCooldown) zeroCooldowns(gm);
        if (oBuildBlock) oBuildBlock(gm, pos, face, itemSource, scale);
        if (SDK::GameMode::g_GameMode_SkipBuildCooldown) zeroCooldowns(gm);
    }

    using continueDestroy_t = unsigned char(__fastcall*)(
        void*, const SDK::IVec3*, unsigned char, float*, unsigned char*);
    continueDestroy_t oContinueDestroy = nullptr;

    unsigned char __fastcall hkContinueDestroy(
        void* gm, const SDK::IVec3* pos, unsigned char face, float* progress, unsigned char* state) {
        SDK::GameMode::remember(gm);
        if (SDK::GameMode::g_GameMode_SkipBuildCooldown) zeroCooldowns(gm);
        const auto result = oContinueDestroy
            ? oContinueDestroy(gm, pos, face, progress, state) : static_cast<unsigned char>(0);
        if (SDK::GameMode::g_GameMode_SkipBuildCooldown) zeroCooldowns(gm);
        return result;
    }
    using gameModeTick_t = void(__fastcall*)(void*);
    gameModeTick_t oGameModeTick = nullptr;

    void __fastcall hkGameModeTick(void* gm) {
        SDK::GameMode::remember(gm);
        if (oGameModeTick) oGameModeTick(gm);
    }
}

void InstallGameModeHooks() {
    const auto& sigs = SigManager::GetInstance();
    const auto install = [](uintptr_t target, void* detour, void** original, const char* name) {
        if (!target || !gh::isExecutablePtr(reinterpret_cast<void*>(target))) return;
        if (HookEngine::Create(reinterpret_cast<void*>(target), detour, original)) return;
        LOG_WARN(std::string("GameMode hook unavailable: ") + name, "GameHooks");
    };

    install(sigs.GameMode_getDestroyRate, &hkGetDestroyRate,
            reinterpret_cast<void**>(&oGetDestroyRate), "FastMine");
    install(sigs.GameMode_buildBlock, &hkBuildBlock,
            reinterpret_cast<void**>(&oBuildBlock), "PlaceDelay");
    install(sigs.GameMode_continueDestroyBlock, &hkContinueDestroy,
            reinterpret_cast<void**>(&oContinueDestroy), "PlaceDelay");
    install(sigs.GameMode_tick, &hkGameModeTick,
            reinterpret_cast<void**>(&oGameModeTick), "GameMode tick");
}
