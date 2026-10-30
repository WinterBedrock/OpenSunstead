// LevelRenderer::renderLevel detour and the render-thread gateways it owns:
// visible third-person rotation smoothing, HUD ticks, deferred chat commands,
// the per-frame FreeCam ghost reset — plus the per-actor nametag builder
// (NameTags "Hide Original"), Actor::attack (combat signal / Switcher) and
// ItemUseSlowdownApply (NoSlow).
#include "GameHooks.h"
#include "HookShared.h"
#include "../SDK/SDK.h"
#include "../SDK/Client/ClientInstance.h"
#include "../SDK/Client/LevelRendererPlayer.h"
#include "../Render/FrameView.h"
#include "../Memory/OffsetManager.h"
#include "../Modules/ModuleManager.h"
#include "../Modules/Combat/Sprint.h"
#include "../Modules/Combat/SilentRotation.h"
#include "../Modules/Combat/CombatSignal.h"
#include "../Modules/Combat/Switcher.h"
#include "../Modules/Movement/NoSlow.h"
#include "../Modules/Visual/Chams.h"
#include "../Modules/Visual/NameTags.h"
#include "../Core/Logger.h"
#include "../Memory/SigManager.h"
#include "../SDK/Actor/MobEffects.h"
#include "MinHook.h"
#include <cmath>
#include <string>

namespace {
    using namespace SDK;

    typedef void(__fastcall* renderLevel_t)(void* lvl, void* scn, void* unk);
    renderLevel_t oRenderLevel = nullptr;

    // Third-person visible-rotation smoothing toward the server-facing aim
    // (SilentRotation), so a spoofed rotation actually shows on the model.
    static constexpr float kVisRotSpeed = 6.0f;

    static bool sehApplyVisibleRotation(void* lvl, void* scn, void* unk) {
        __try {
            const int persp = gh::g_lastPerspective.load(std::memory_order_relaxed);
            const bool thirdPerson = (persp == 1 || persp == 2);

            SDK::ClientInstance* ci = SDK::ClientInstance::get();
            if (!ci) return false;
            SDK::Actor* lp = ci->getLocalPlayer();
            if (!lp) return false;
            ActorRotationComponent* rc = lp->getRotation();
            if (!rc) return false;

            const float realYaw = rc->rotation.y;
            const float realPitch = rc->rotation.x;

            float tyaw = 0.0f, tpitch = 0.0f;
            const bool haveTarget = SilentRotation::Peek(tyaw, tpitch);
            const float destYaw = haveTarget ? tyaw : realYaw;
            const float destPitch = haveTarget ? tpitch : realPitch;
            if (!std::isfinite(destYaw) || !std::isfinite(destPitch)) return false;

            // Frame-rate independent exponential smoothing.
            static bool s_inited = false;
            static float s_lYaw = 0.0f, s_lPitch = 0.0f;
            static ULONGLONG s_last = 0;
            ULONGLONG now = GetTickCount64();
            float dt = s_inited ? (float)(now - s_last) / 1000.0f : 0.0f;
            s_last = now;
            if (dt > 0.1f) dt = 0.1f;
            if (!s_inited) { s_lYaw = destYaw; s_lPitch = destPitch; s_inited = true; }

            float dyaw = destYaw - s_lYaw;
            while (dyaw > 180.0f) dyaw -= 360.0f;
            while (dyaw < -180.0f) dyaw += 360.0f;
            float k = 1.0f - expf(-dt * kVisRotSpeed);
            s_lYaw += dyaw * k;
            s_lPitch += (destPitch - s_lPitch) * k;
            while (s_lYaw > 180.0f) s_lYaw -= 360.0f;
            while (s_lYaw < -180.0f) s_lYaw += 360.0f;

            // Nothing to show: no active aim and the smoothed value settled
            // back onto the real rotation — except in third person, where the
            // drawn model always animates.
            const float settle = fabsf(dyaw) + fabsf(destPitch - s_lPitch);
            if (!thirdPerson) return false;
            if (!haveTarget && settle < 0.05f) return false;

            const float sPitch = rc->rotation.x, sOldPitch = rc->rotationOld.x;
            rc->rotation.x = s_lPitch; rc->rotationOld.x = s_lPitch;

            ActorHeadRotationComponent* hc = lp->getHeadRotation();
            bool haveHead = false; float sHead = 0, sOldHead = 0;
            if (hc && std::isfinite(hc->mHeadRot)) {
                haveHead = true; sHead = hc->mHeadRot; sOldHead = hc->mOldHeadRot;
                hc->mHeadRot = s_lYaw; hc->mOldHeadRot = s_lYaw;
            }
            MobBodyRotationComponent* bc = lp->getBodyRotation();
            bool haveBody = false; float sBody = 0, sOldBody = 0;
            if (bc && std::isfinite(bc->yBodyRot)) {
                haveBody = true; sBody = bc->yBodyRot; sOldBody = bc->yOldBodyRot;
                bc->yBodyRot = s_lYaw; bc->yOldBodyRot = s_lYaw;
            }

            oRenderLevel(lvl, scn, unk);

            rc->rotation.x = sPitch; rc->rotationOld.x = sOldPitch;
            if (haveHead) { hc->mHeadRot = sHead; hc->mOldHeadRot = sOldHead; }
            if (haveBody) { bc->yBodyRot = sBody; bc->yOldBodyRot = sOldBody; }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // SEH-guarded HUD tick: a bad pointer must never take the game down on
    // world entry.
    static bool tryOnTickImpl() {
        __try {
            ModuleManager::GetInstance().OnTick();
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static void tryOnTick() {
        if (!tryOnTickImpl()) {
            static int tickCrash = 0;
            if (++tickCrash == 1) LOG_WARN("OnTick threw in renderLevel (suppressed)", "GameHooks");
        }
    }

    void __fastcall hkLevelRenderer_renderLevel(void* lvl, void* scn, void* unk) {
        // If the hook lost the trampoline, bail out before calling through a
        // null pointer — better to render nothing than crash on world entry.
        if (!oRenderLevel) return;

        // New world-render frame: the FreeCam ghost body may be injected once.
        GameHooks::ResetFreeCamGhostFrame();

        // FOV module scaling (Zoom / Sprint kick): this is the single writer
        // for the LRP projection focal elements, applied against the game's
        // CURRENT base renormalized, BEFORE the world draws with it.
        SDK::sehApplyFovMod(g_zoomProjScale * g_sprintFovScale);

        // Re-assert sprint AFTER tick validation cleared it, BEFORE the
        // model/FOV read it.
        {
            Module* sp = ModuleManager::GetInstance().GetModuleByName("Sprint");
            if (sp && sp->IsEnabled())
                static_cast<Sprint*>(sp)->PreRenderAssert();
        }

        if (!sehApplyVisibleRotation(lvl, scn, unk))
            oRenderLevel(lvl, scn, unk);

        gh::runDeferredChatCommands();
        gh::tryResolveAndSwapPickRange();
        tryOnTick();

        // Re-apply with the FRESH multipliers OnTick just published (renormalized,
        // so a no-change double-apply is a no-op).
        SDK::sehApplyFovMod(g_zoomProjScale * g_sprintFovScale);

        gh::ensureNoFireIsOnFireHook();

        // FreeCam ghost fallback: no entity dispatch happened inside the world
        // render → re-use the cached dispatcher arguments of the last real one.
        Chams::InjectFreeCamGhostCached();
    }

    // ---- per-actor nametag builder (NOT the per-frame renderNametag pass —
    // that one owns its out-vector, so a hook there could hide nothing).
    // Vanilla "no nametag" = zero the 24-byte out vector and return it; the
    // caller appends nothing. Actor arrives as arg5.
    typedef void* (__fastcall* nametagBuildForActor_t)(void* fontCtx, void* outVector,
                                                        void* layout, void* dataProvider,
                                                        void* actor, void* name,
                                                        void* pos3, float scaleish,
                                                        void* renderParams);
    static nametagBuildForActor_t oNametagBuildForActor = nullptr;

    static void* __fastcall hkNametagBuildForActor(void* fontCtx, void* outVector,
                                                    void* layout, void* dataProvider,
                                                    void* actor, void* name,
                                                    void* pos3, float scaleish,
                                                    void* renderParams) {
        if (!oNametagBuildForActor) return outVector;   // trampoline lost

        bool hideThis = false;
        __try { hideThis = NameTags::ShouldHideActor(actor); }
        __except (EXCEPTION_EXECUTE_HANDLER) { hideThis = false; }

        if (hideThis && outVector) {
            *reinterpret_cast<uint64_t*>((char*)outVector +  0) = 0;
            *reinterpret_cast<uint64_t*>((char*)outVector +  8) = 0;
            *reinterpret_cast<uint64_t*>((char*)outVector + 16) = 0;
            return outVector;
        }
        return oNametagBuildForActor(fontCtx, outVector, layout, dataProvider,
                                     actor, name, pos3, scaleish, renderParams);
    }

    // NoSlow: cancel the item-use movement slowdown.
    typedef void (__fastcall* itemUseSlowdown_t)(void* a1, float* factor, void* move, int* out);
    static itemUseSlowdown_t oItemUseSlowdown = nullptr;

    static void __fastcall hkItemUseSlowdown(void* a1, float* factor, void* move, int* out) {
        bool cancel = false;
        __try { cancel = NoSlow::ShouldCancel(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { cancel = false; }

        if (cancel) { if (out) *out = 0; return; }
        if (oItemUseSlowdown) oItemUseSlowdown(a1, factor, move, out);
    }

    // Actor::attack — TargetHUD hit flash + Switcher's pre-attack slot swap.
    // The weapon's MobEquipmentPacket must go out BEFORE the attack
    // transaction is serialized; the slot restore is Switcher::OnUpdate's job.
    typedef void* (__fastcall* actorAttack_t)(void* obj, void* a2, void* target, void* cause, void* a5);
    static actorAttack_t oActorAttack = nullptr;

    static void switcherAttackPhase(SDK::Actor* lp) {
        Module* sw = ModuleManager::GetInstance().GetModuleByName("Switcher");
        if (!sw || !sw->IsEnabled()) return;
        static_cast<Switcher*>(sw)->OnLocalAttack(lp);
    }

    static void* __fastcall hkActorAttack(void* obj, void* a2, void* target, void* cause, void* a5) {
        if (!oActorAttack) return nullptr;
        __try {
            SDK::ClientInstance* ci = SDK::ClientInstance::get();
            if (ci) {
                SDK::Actor* lp = ci->getLocalPlayer();
                if (obj && lp && obj == (void*)lp) {
                    if (target) CombatSignal::registerHit(target);
                    switcherAttackPhase(lp);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return oActorAttack(obj, a2, target, cause, a5);
    }
}

void InstallRenderLevelHook() {
    // Hook LevelRenderer::renderLevel.
    uintptr_t renderLevelAddr = SigManager::GetInstance().LevelRenderer_renderLevel;
    if (renderLevelAddr) {
        MH_STATUS ch = MH_CreateHook(reinterpret_cast<LPVOID>(renderLevelAddr),
                                     &hkLevelRenderer_renderLevel,
                                     reinterpret_cast<LPVOID*>(&oRenderLevel));
        if (ch == MH_OK || ch == MH_ERROR_ALREADY_CREATED) {
            MH_STATUS eh = gh::HookEnable(reinterpret_cast<LPVOID>(renderLevelAddr));
            if (eh == MH_OK && oRenderLevel) {
                LOG_INFO("world-render hook installed @ " + std::to_string(renderLevelAddr), "GameHooks");
            } else {
                LOG_ERROR("FAILED to enable world-render hook (MH_EnableHook=" + std::to_string((int)eh) +
                          ", oRenderLevel=" + std::to_string(reinterpret_cast<uintptr_t>(oRenderLevel)) + ")", "GameHooks");
                // Clear the trampoline so the detour bails out instead of
                // calling through a null function pointer.
                oRenderLevel = nullptr;
            }
        } else {
            LOG_ERROR("FAILED to create world-render hook (MH_CreateHook=" + std::to_string((int)ch) +
                      " @ " + std::to_string(renderLevelAddr) + ")", "GameHooks");
            oRenderLevel = nullptr;
        }
    } else {
        LOG_WARN("world-render signature not found; detour disabled", "GameHooks");
    }
}

void InstallNametagHook() {
    uintptr_t nametagActorAddr = SigManager::GetInstance().Nametag_buildForActor;
    if (nametagActorAddr) {
        if (MH_CreateHook(reinterpret_cast<LPVOID>(nametagActorAddr),
                          &hkNametagBuildForActor,
                          reinterpret_cast<LPVOID*>(&oNametagBuildForActor)) == MH_OK) {
            gh::HookEnable(reinterpret_cast<LPVOID>(nametagActorAddr));
            LOG_INFO("NameTag_buildForActor hooked (NameTags can hide the original tag)", "GameHooks");
        } else {
            LOG_ERROR("FAILED to hook NameTag_buildForActor", "GameHooks");
        }
    } else {
        LOG_WARN("NameTag_buildForActor signature not found; Hide Original disabled", "GameHooks");
    }
}

void InstallAttackHook() {
    uintptr_t attackAddr = SigManager::GetInstance().Actor_attack;
    if (attackAddr) {
        if (MH_CreateHook(reinterpret_cast<LPVOID>(attackAddr), &hkActorAttack,
                          reinterpret_cast<LPVOID*>(&oActorAttack)) == MH_OK) {
            gh::HookEnable(reinterpret_cast<LPVOID>(attackAddr));
            LOG_INFO("Actor::attack hooked (combat hit signal)", "GameHooks");
        } else {
            LOG_ERROR("FAILED to hook Actor::attack", "GameHooks");
            oActorAttack = nullptr;
        }
    } else {
        LOG_WARN("Actor::attack signature not found; manual-hit feedback disabled", "GameHooks");
    }
}

void InstallSlowdownHook() {
    uintptr_t slowdownAddr = SigManager::GetInstance().ItemUseSlowdownApply;
    if (slowdownAddr) {
        if (MH_CreateHook(reinterpret_cast<LPVOID>(slowdownAddr), &hkItemUseSlowdown,
                          reinterpret_cast<LPVOID*>(&oItemUseSlowdown)) == MH_OK) {
            gh::HookEnable(reinterpret_cast<LPVOID>(slowdownAddr));
            LOG_INFO("ItemUseSlowdownApply hooked (NoSlow available)", "GameHooks");
        } else {
            LOG_ERROR("FAILED to hook ItemUseSlowdownApply", "GameHooks");
            oItemUseSlowdown = nullptr;
        }
    } else {
        LOG_WARN("ItemUseSlowdownApply signature not found; NoSlow disabled", "GameHooks");
    }
}
