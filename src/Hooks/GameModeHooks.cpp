// GameMode detours: Reach (getPickRange), FastMine (getDestroyRate) and
// PlaceDelay (buildBlock / continueDestroyBlock cooldown zeroing).
#include "GameHooks.h"
#include "HookShared.h"
#include "../SDK/SDK.h"
#include "../SDK/Client/ClientInstance.h"
#include "../SDK/Client/GameMode.h"
#include "../SDK/SDKRuntime.h"
#include "../Memory/OffsetManager.h"
#include "../Modules/ModuleManager.h"
#include "../Modules/Combat/Reach.h"
#include "../Modules/Player/FastMine.h"
#include "../Core/Logger.h"
#include "../Memory/SigManager.h"
#include "MinHook.h"
#include <string>

namespace {
    using namespace SDK;

    // GameMode::getPickRange is `float (GameMode*, int* inputMode)` on
    // 1.26.50+ (it dereferences the out-param). vtable slot 10.
    typedef float(__fastcall* getPickRange_t)(void* _this, int* inputMode);
    void* oSurvivalGetPickRange = nullptr;
    void* oCreativeGetPickRange = nullptr;

    static float applyReach(float ret) {
        SDK::ClientInstance* cInst = SDK::ClientInstance::get();
        if (!cInst) return ret;
        SDK::Actor* lp = cInst->getLocalPlayer();
        if (!lp) return ret;
        Module* reachMod = ModuleManager::GetInstance().GetModuleByName("Reach");
        if (reachMod && reachMod->IsEnabled()) {
            const float customReach = static_cast<Reach*>(reachMod)->GetReachDistance();
            if (ret < customReach) ret = customReach;
        }
        return ret;
    }

    float __fastcall hkSurvivalGetPickRange(void* _this, int* inputMode) {
        float ret = oSurvivalGetPickRange
            ? reinterpret_cast<getPickRange_t>(oSurvivalGetPickRange)(_this, inputMode)
            : 0.0f;
        return applyReach(ret);
    }

    float __fastcall hkCreativeGetPickRange(void* _this, int* inputMode) {
        float ret = oCreativeGetPickRange
            ? reinterpret_cast<getPickRange_t>(oCreativeGetPickRange)(_this, inputMode)
            : 0.0f;
        return applyReach(ret);
    }

    // Warm the dynamic Actor_mGameMode offset (0x800..0xC00 probe: the field
    // points to an object whose vtable slot 10 is the pick-range getter).
    // Slot 11 is NOT a use-item overload on this build — probing it would
    // accept any GameMode-shaped object.
    uintptr_t resolveGameModeOffset(void* lp) {
        for (uintptr_t offset = 0x800; offset <= 0xC00; offset += 8) {
            if (!sdk_safe_ptr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(lp) + offset), sizeof(void*)))
                continue;
            void* ptr = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(lp) + offset);
            if (ptr && sdk_safe_ptr(ptr, sizeof(void*))) {
                void** vtable = *reinterpret_cast<void***>(ptr);
                if (vtable && sdk_safe_ptr(vtable, sizeof(void*) * 11)) {
                    void* getPickRangeFunc = vtable[10];
                    if (getPickRangeFunc && gh::isExecutablePtr(getPickRangeFunc)) {
                        LOG_INFO("Dynamically resolved Actor_mGameMode offset: 0x" + std::to_string(offset), "GameHooks");
                        return offset;
                    }
                }
            }
        }
        LOG_WARN("Failed to dynamically resolve Actor_mGameMode offset. Using fallback 0xAA0", "GameHooks");
        return 0xAA0;
    }

    // SEH-guarded: resolve & swap the GameMode vtable's getPickRange slot.
    // Retries per frame from renderLevel until both vtables land.
    static int tryResolveAndSwapPickRangeImpl() {
        __try {
            SDK::ClientInstance* cInst = SDK::ClientInstance::get();
            if (!cInst) return 0;
            SDK::Actor* lp = cInst->getLocalPlayer();
            if (!lp) return 0;

            if (Offsets().Actor_mGameMode == 0) {
                int resolved = static_cast<int>(resolveGameModeOffset(lp));
                if (resolved == 0) resolved = 0xAA0;
                Offsets().setActorGameMode(resolved);
            }

            uintptr_t lpBase = reinterpret_cast<uintptr_t>(lp);
            const int gmOff = Offsets().Actor_mGameMode;
            if (!sdk_safe_ptr(reinterpret_cast<void*>(lpBase + gmOff), sizeof(void*))) return 0;
            void* gm = *reinterpret_cast<void**>(lpBase + gmOff);
            if (!gm || !sdk_safe_ptr(gm, sizeof(void*))) return 0;
            void** vtable = *reinterpret_cast<void***>(gm);
            if (!vtable || !sdk_safe_ptr(vtable, sizeof(void*) * 11)) return 0;

            uintptr_t getPickRangeAddr = reinterpret_cast<uintptr_t>(vtable[10]);
            if (!getPickRangeAddr) return 0;
            if (getPickRangeAddr == reinterpret_cast<uintptr_t>(reinterpret_cast<void*>(&hkSurvivalGetPickRange))) return 0;
            if (getPickRangeAddr == reinterpret_cast<uintptr_t>(reinterpret_cast<void*>(&hkCreativeGetPickRange))) return 0;

            DWORD oldProtect;
            if (!VirtualProtect(&vtable[10], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) return 0;

            if (!oSurvivalGetPickRange) {
                oSurvivalGetPickRange = reinterpret_cast<void*>(getPickRangeAddr);
                vtable[10] = reinterpret_cast<void*>(&hkSurvivalGetPickRange);
                VirtualProtect(&vtable[10], sizeof(void*), oldProtect, &oldProtect);
                return 1;
            } else if (!oCreativeGetPickRange && getPickRangeAddr != reinterpret_cast<uintptr_t>(oSurvivalGetPickRange)) {
                oCreativeGetPickRange = reinterpret_cast<void*>(getPickRangeAddr);
                vtable[10] = reinterpret_cast<void*>(&hkCreativeGetPickRange);
                VirtualProtect(&vtable[10], sizeof(void*), oldProtect, &oldProtect);
                return 2;
            }
            VirtualProtect(&vtable[10], sizeof(void*), oldProtect, &oldProtect);
            return 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
    }

    // The vanilla break pipeline: continueDestroyBlock accumulates
    // elapsed/0.05 * rate and finishes at >= 0.99998999 — haste and mining
    // fatigue stay inside the rate because the game computes them there.
    typedef float(__fastcall* getDestroyRate_t)(void* gm, void* block);
    static getDestroyRate_t oGetDestroyRate = nullptr;

    static float __fastcall hkGetDestroyRate(void* gm, void* block) {
        float rate = oGetDestroyRate ? oGetDestroyRate(gm, block) : 0.f;
        Module* fm = ModuleManager::GetInstance().GetModuleByName("FastMine");
        if (fm && fm->IsEnabled()) {
            float mult = static_cast<FastMine*>(fm)->GetMultiplier();
            if (mult < 0.1f) mult = 0.1f;
            if (mult > 50.0f) mult = 50.0f;
            rate *= mult;
        }
        return rate;
    }

    // ---- PlaceDelay cooldown zeroing ----------------------------------------
    // TWO stamps gate the hold path: gm+0x88 (BuildCooldownLastNs, rewritten
    // by buildBlock) and gm+0x90 (UseRepeatNextNs, written by
    // continueDestroyBlock). Both must go, before AND after the calls — the
    // leftovers are exactly the held-place delay.

    // Cheap "this really is a GameMode" bound if a signature ever drifts
    // again: the destroy/build slots are adjacent and non-null code pointers.
    static bool looksLikeGameMode(void* gm) {
        if (!gm || !sdk_safe_ptr(gm, 8)) return false;
        void** vt = *reinterpret_cast<void***>(gm);
        if (!vt || !sdk_safe_ptr(vt, ::Layout::VTable::Guard(::Layout::VTable::GameMode_buildBlock + 1)))
            return false;
        void* bb = vt[::Layout::VTable::GameMode_buildBlock];
        void* cd = vt[::Layout::VTable::GameMode_continueDestroy];
        return bb && sdk_safe_ptr(bb, 16) && cd && sdk_safe_ptr(cd, 16);
    }

    static void zeroGmCooldowns(void* gm) {
        if (!gm || !looksLikeGameMode(gm)) return;
        if (!sdk_safe_ptr(gm, ::Layout::GameMode::UseRepeatIntervalMs + 8)) return;
        __try {
            const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(gm);
            *reinterpret_cast<std::uint64_t*>(base + ::Layout::GameMode::BuildCooldownLastNs) = 0;
            *reinterpret_cast<std::uint64_t*>(base + ::Layout::GameMode::UseRepeatNextNs) = 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    typedef void(__fastcall* buildBlock_t)(void* gm, const SDK::IVec3* pos, unsigned char face, bool itemSrc, float cooldownScale);
    static buildBlock_t oBuildBlock = nullptr;

    static void __fastcall hkBuildBlock(void* gm, const SDK::IVec3* pos, unsigned char face, bool itemSrc, float cooldownScale) {
        if (SDK::GameMode::g_GameMode_SkipBuildCooldown)
            zeroGmCooldowns(gm);
        if (oBuildBlock) oBuildBlock(gm, pos, face, itemSrc, cooldownScale);
        if (SDK::GameMode::g_GameMode_SkipBuildCooldown)
            zeroGmCooldowns(gm);
    }

    typedef unsigned char(__fastcall* continueDestroyBlock_t)(
        void* gm, const SDK::IVec3* pos, unsigned char face, float* a4, unsigned char* a5);
    static continueDestroyBlock_t oContinueDestroyBlock = nullptr;

    static unsigned char __fastcall hkContinueDestroyBlock(
        void* gm, const SDK::IVec3* pos, unsigned char face, float* a4, unsigned char* a5) {
        if (SDK::GameMode::g_GameMode_SkipBuildCooldown)
            zeroGmCooldowns(gm);
        unsigned char r = oContinueDestroyBlock ? oContinueDestroyBlock(gm, pos, face, a4, a5) : 0;
        if (SDK::GameMode::g_GameMode_SkipBuildCooldown)
            zeroGmCooldowns(gm);
        return r;
    }
}

void gh::tryResolveAndSwapPickRange() {
    const int swapKind = tryResolveAndSwapPickRangeImpl();
    if (swapKind < 0) {
        static int gmCrash = 0;
        if (++gmCrash == 1) LOG_WARN("getPickRange resolution threw (suppressed), vtable not swapped", "GameHooks");
    } else if (swapKind == 1) {
        LOG_INFO("Vtable swapped getPickRange 1 (Survival)", "GameHooks");
    } else if (swapKind == 2) {
        LOG_INFO("Vtable swapped getPickRange 2 (Creative)", "GameHooks");
    }
}

void InstallGameModeHooks() {
    // FastMine: GameMode::getDestroyRate.
    {
        uintptr_t drAddr = g_GameMode_getDestroyRate;
        if (drAddr && MH_CreateHook(reinterpret_cast<LPVOID>(drAddr), &hkGetDestroyRate,
                                    reinterpret_cast<LPVOID*>(&oGetDestroyRate)) == MH_OK) {
            gh::HookEnable(reinterpret_cast<LPVOID>(drAddr));
            LOG_INFO("GameMode::getDestroyRate hooked (FastMine)", "GameHooks");
        } else {
            LOG_WARN("GameMode::getDestroyRate NOT hooked — FastMine inactive", "GameHooks");
        }
    }

    // PlaceDelay: GameMode::buildBlock.
    {
        uintptr_t bbAddr = SigManager::GetInstance().GameMode_buildBlock;
        if (bbAddr && MH_CreateHook(reinterpret_cast<LPVOID>(bbAddr), &hkBuildBlock,
                                    reinterpret_cast<LPVOID*>(&oBuildBlock)) == MH_OK) {
            gh::HookEnable(reinterpret_cast<LPVOID>(bbAddr));
            LOG_INFO("GameMode::buildBlock hooked (PlaceDelay)", "GameHooks");
        } else {
            LOG_WARN("GameMode::buildBlock NOT hooked — PlaceDelay only affects our callers", "GameHooks");
        }
    }

    // PlaceDelay hold path: GameMode::continueDestroyBlock.
    {
        uintptr_t cdbAddr = SigManager::GetInstance().GameMode_continueDestroyBlock;
        if (cdbAddr && MH_CreateHook(reinterpret_cast<LPVOID>(cdbAddr), &hkContinueDestroyBlock,
                                     reinterpret_cast<LPVOID*>(&oContinueDestroyBlock)) == MH_OK) {
            gh::HookEnable(reinterpret_cast<LPVOID>(cdbAddr));
            LOG_INFO("GameMode::continueDestroyBlock hooked (PlaceDelay hold path)", "GameHooks");
        } else {
            LOG_WARN("GameMode::continueDestroyBlock NOT hooked — hold-to-place delay may persist", "GameHooks");
        }
    }
}
