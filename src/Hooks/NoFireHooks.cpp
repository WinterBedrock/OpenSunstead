// NoFire detours: the four fire-particle draw paths, the molang
// query.is_on_fire gate and the Actor::isOnFire vtable slot — each is one of
// the independent paths the first-person burning overlay renders through.
#include "GameHooks.h"
#include "HookShared.h"
#include "../SDK/SDK.h"
#include "../SDK/Client/ClientInstance.h"
#include "../Modules/ModuleManager.h"
#include "../Core/Logger.h"
#include "../Memory/SigManager.h"
#include "MinHook.h"
#include <cstdio>
#include <string>

namespace {
    using namespace SDK;

    static Module* noFireModule() {
        static Module* s_noFire = nullptr;
        if (!s_noFire)
            s_noFire = ModuleManager::GetInstance().GetModuleByName("NoFire");
        return s_noFire;
    }

    typedef void(__fastcall* FlameRenderer_draw_t)(void* a1, void* a2);
    static FlameRenderer_draw_t oFlameRendererDraw = nullptr;

    static void __fastcall hkFlameRendererDraw(void* a1, void* a2) {
        Module* nf = noFireModule();
        if (nf && nf->IsEnabled()) return;
        oFlameRendererDraw(a1, a2);
    }

    typedef void(__fastcall* OnFireClientSystem_update_t)(void* a1, void* a2);
    static OnFireClientSystem_update_t oOnFireClientSystemUpdate = nullptr;

    static void __fastcall hkOnFireClientSystemUpdate(void* a1, void* a2) {
        Module* nf = noFireModule();
        if (nf && nf->IsEnabled()) return;
        oOnFireClientSystemUpdate(a1, a2);
    }

    typedef void(__fastcall* ClientFireActorEvent_handleFire_t)(void* a1, void* a2, void* a3, void* a4);
    static ClientFireActorEvent_handleFire_t oClientFireActorEvent = nullptr;

    static void __fastcall hkClientFireActorEvent(void* a1, void* a2, void* a3, void* a4) {
        Module* nf = noFireModule();
        if (nf && nf->IsEnabled()) return;
        oClientFireActorEvent(a1, a2, a3, a4);
    }

    typedef __int64(__fastcall* FlameBillboard_draw_t)(void* a1, void* a2, void* a3, void* a4);
    static FlameBillboard_draw_t oFlameBillboardDraw = nullptr;

    static __int64 __fastcall hkFlameBillboardDraw(void* a1, void* a2, void* a3, void* a4) {
        Module* nf = noFireModule();
        if (nf && nf->IsEnabled()) return 0;
        return oFlameBillboardDraw(a1, a2, a3, a4);
    }

    // Molang query.is_on_fire: rdx = eval context (actor at +0x38), returns a
    // pointer to a static molang value blob (1.0 / 0.0) in rax.
    typedef void* (__fastcall* MolangIsOnFire_t)(void* rcx, void* evalCtx);
    static MolangIsOnFire_t oMolangIsOnFire = nullptr;
    static void* s_molangFalseBlob = nullptr;   // the static 0.0 result (captured once)

    static void* sehMolangEvalActor(void* evalCtx) {
        __try {
            return *(void**)((uintptr_t)evalCtx + 0x38);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // A zeroed context has a null actor, and the handler's own null path
    // returns exactly the 0.0 blob.
    static void* sehMolangCaptureFalse(MolangIsOnFire_t original) {
        __try {
            alignas(16) unsigned char zeroCtx[0x40] = { 0 };
            return original(nullptr, zeroCtx);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    static void* __fastcall hkMolangIsOnFire(void* rcx, void* evalCtx) {
        void* result = oMolangIsOnFire(rcx, evalCtx);

        Module* nf = noFireModule();
        if (!nf || !nf->IsEnabled()) return result;

        void* actor = sehMolangEvalActor(evalCtx);
        if (!actor) return result;
        if (!s_molangFalseBlob) s_molangFalseBlob = sehMolangCaptureFalse(oMolangIsOnFire);
        if (!s_molangFalseBlob) return result;

        SDK::ClientInstance* ci = SDK::ClientInstance::get();
        if (!ci) return result;
        SDK::Actor* lp = ci->getLocalPlayer();
        if (actor == (void*)lp) {
            static unsigned long long s_lastWinMs = 0;
            static unsigned s_winHits = 0;
            ++s_winHits;
            const unsigned long long nowMs = GetTickCount64();
            if (nowMs - s_lastWinMs >= 2000ull) {
                char dbg[96];
                std::snprintf(dbg, sizeof(dbg),
                              "NoFire: molang falsified %u local is_on_fire evals in window",
                              s_winHits);
                LOG_INFO(dbg, "NoFire");
                s_winHits = 0;
                s_lastWinMs = nowMs;
            }
            result = s_molangFalseBlob;
        }
        return result;
    }

    // Actor::isOnFire — vtable slot 0x178 on the local player. Installed
    // lazily once a live local player exists (renderLevel retries it).
    typedef bool(__fastcall* Actor_isOnFire_t)(void* actor);
    static Actor_isOnFire_t oActorIsOnFire = nullptr;
    static void* s_actorIsOnFireTarget = nullptr;

    static bool __fastcall hkActorIsOnFire(void* actor) {
        Module* nf = noFireModule();
        if (nf && nf->IsEnabled() && actor) {
            SDK::ClientInstance* ci = SDK::ClientInstance::get();
            if (ci) {
                SDK::Actor* lp = ci->getLocalPlayer();
                if (lp && actor == (void*)lp) return false;
            }
        }
        return oActorIsOnFire ? oActorIsOnFire(actor) : true;
    }

    static bool sehInstallActorIsOnFireHook() {
        __try {
            SDK::ClientInstance* ci = SDK::ClientInstance::get();
            if (!ci || !sdk_safe_ptr(ci, sizeof(void*))) return false;
            SDK::Actor* lp = ci->getLocalPlayer();
            if (!lp || !sdk_safe_ptr(lp, sizeof(void*) * (0x178 / 8 + 1))) return false;
            void** vt = *(void***)lp;
            if (!vt || !sdk_safe_ptr(vt, sizeof(void*) * (0x178 / 8 + 1))) return false;
            void* fn = vt[0x178 / 8];
            if (!fn || !gh::isExecutablePtr(fn)) return false;
            if (fn == s_actorIsOnFireTarget) return true;   // already installed
            if (MH_CreateHook(fn, &hkActorIsOnFire,
                              reinterpret_cast<LPVOID*>(&oActorIsOnFire)) != MH_OK)
                return false;
            if (gh::HookEnable(fn) != MH_OK) return false;
            s_actorIsOnFireTarget = fn;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
}

void gh::ensureNoFireIsOnFireHook() {
    static bool s_installed = false;
    if (s_installed) return;
    if (sehInstallActorIsOnFireHook()) {
        s_installed = true;
        LOG_INFO("Actor::isOnFire (vtable slot 0x178) hooked — NoFire FP overlay", "GameHooks");
    }
}

void InstallNoFireHooks() {
    {
        uintptr_t flameAddr = SigManager::GetInstance().FlameRenderer_draw;
        if (flameAddr) {
            if (MH_CreateHook(reinterpret_cast<LPVOID>(flameAddr), &hkFlameRendererDraw,
                    reinterpret_cast<LPVOID*>(&oFlameRendererDraw)) == MH_OK) {
                gh::HookEnable(reinterpret_cast<LPVOID>(flameAddr));
                LOG_INFO("FlameRenderer::draw hooked (NoFire)", "GameHooks");
            } else {
                LOG_ERROR("FAILED to hook FlameRenderer::draw", "GameHooks");
            }
        } else {
            LOG_WARN("FlameRenderer::draw signature not found; NoFire will not work", "GameHooks");
        }
    }

    {
        uintptr_t onFireAddr = SigManager::GetInstance().OnFireClientSystem_update;
        if (onFireAddr) {
            if (MH_CreateHook(reinterpret_cast<LPVOID>(onFireAddr), &hkOnFireClientSystemUpdate,
                    reinterpret_cast<LPVOID*>(&oOnFireClientSystemUpdate)) == MH_OK) {
                gh::HookEnable(reinterpret_cast<LPVOID>(onFireAddr));
                LOG_INFO("OnFireClientSystem::update hooked (NoFire self overlay)", "GameHooks");
            } else {
                LOG_ERROR("FAILED to hook OnFireClientSystem::update", "GameHooks");
            }
        } else {
            LOG_WARN("OnFireClientSystem::update signature not found; NoFire self overlay inactive", "GameHooks");
        }
    }

    {
        uintptr_t fireEvtAddr = SigManager::GetInstance().ClientFireActorEvent_handleFire;
        if (fireEvtAddr) {
            if (MH_CreateHook(reinterpret_cast<LPVOID>(fireEvtAddr), &hkClientFireActorEvent,
                    reinterpret_cast<LPVOID*>(&oClientFireActorEvent)) == MH_OK) {
                gh::HookEnable(reinterpret_cast<LPVOID>(fireEvtAddr));
                LOG_INFO("ClientFireActorEventSystem::handleFire hooked (NoFire FP overlay)", "GameHooks");
            } else {
                LOG_ERROR("FAILED to hook ClientFireActorEventSystem::handleFire", "GameHooks");
            }
        } else {
            LOG_WARN("ClientFireActorEventSystem::handleFire signature not found; NoFire FP overlay inactive", "GameHooks");
        }
    }

    {
        uintptr_t molangAddr = SigManager::GetInstance().MolangQuery_isOnFire;
        if (molangAddr) {
            if (MH_CreateHook(reinterpret_cast<LPVOID>(molangAddr), &hkMolangIsOnFire,
                    reinterpret_cast<LPVOID*>(&oMolangIsOnFire)) == MH_OK) {
                gh::HookEnable(reinterpret_cast<LPVOID>(molangAddr));
                LOG_INFO("Molang query.is_on_fire hooked (NoFire FP overlay)", "GameHooks");
            } else {
                LOG_ERROR("FAILED to hook Molang query.is_on_fire", "GameHooks");
            }
        } else {
            LOG_WARN("Molang query.is_on_fire signature not found; NoFire FP overlay inactive", "GameHooks");
        }
    }

    {
        uintptr_t bbAddr = SigManager::GetInstance().FlameBillboard_draw;
        if (bbAddr) {
            if (MH_CreateHook(reinterpret_cast<LPVOID>(bbAddr), &hkFlameBillboardDraw,
                    reinterpret_cast<LPVOID*>(&oFlameBillboardDraw)) == MH_OK) {
                gh::HookEnable(reinterpret_cast<LPVOID>(bbAddr));
                LOG_INFO("FlameBillboard::draw hooked (NoFire FP overlay)", "GameHooks");
            } else {
                LOG_ERROR("FAILED to hook FlameBillboard::draw", "GameHooks");
            }
        } else {
            LOG_WARN("FlameBillboard::draw signature not found; NoFire FP overlay incomplete", "GameHooks");
        }
    }
}
