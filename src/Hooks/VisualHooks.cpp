// Option/world-level visual hooks: Options::getGamma (Fullbright),
// Options::getPerspective + _updatePlayer (Freelook, module OnUpdate
// dispatch), Dimension::getTimeOfDay / getSkyColor (TimeChanger / FogColor).
#include "GameHooks.h"
#include "HookShared.h"
#include "../SDK/SDK.h"
#include "../SDK/Client/ClientInstance.h"
#include "../Modules/ModuleManager.h"
#include "../Modules/Visual/Fullbright.h"
#include "../Modules/Visual/TimeChanger.h"
#include "../Modules/Visual/FogColor.h"
#include "../Modules/Player/Freelook.h"
#include "../Utils/MiscUtils/ColorUtils.hpp"
#include "../Core/Logger.h"
#include "../Memory/SigManager.h"
#include "MinHook.h"
#include <atomic>
#include <imgui.h>

namespace gh {
    std::atomic<int> g_lastPerspective{ 0 };
}

namespace {
    using namespace SDK;

    // Options::getGamma — Fullbright gamma override.
    typedef float(__fastcall* Options_getGamma_t)(void*, void*);
    static Options_getGamma_t oOptionsGetGamma = nullptr;

    static float __fastcall hkOptionsGetGamma(void* options, void* a2) {
        float gamma = oOptionsGetGamma(options, a2);
        Module* fbMod = ModuleManager::GetInstance().GetModuleByName("Fullbright");
        if (fbMod && fbMod->IsEnabled())
            return static_cast<Fullbright*>(fbMod)->GetGamma();
        return gamma;
    }

    // Options::getPerspective — force third-person-back (1) while Freelook is
    // active. Perspective values: 0=first, 1=third-back, 2=third-front.
    typedef int(__fastcall* getPerspective_t)(void* options);
    static getPerspective_t oGetPerspective = nullptr;
    static void* s_perspectiveOptions = nullptr;

    static int __fastcall hkGetPerspective(void* options) {
        // Remember the Options object: FreeCam's ghost injection reads the
        // perspective through the REAL getter so a stale mirror can never
        // gate it.
        s_perspectiveOptions = options;
        Module* fl = ModuleManager::GetInstance().GetModuleByName("Freelook");
        if (fl && static_cast<Freelook*>(fl)->IsActive()) {
            gh::g_lastPerspective.store(1, std::memory_order_relaxed);
            return 1;
        }
        int p = oGetPerspective ? oGetPerspective(options) : 0;
        gh::g_lastPerspective.store(p, std::memory_order_relaxed);
        return p;
    }

    // UpdatePlayerFromCameraSystemUtil::_updatePlayer — freeze camera look
    // angles while Freelook is active + dispatch module OnUpdate (game thread).
    typedef void(__fastcall* updatePlayer_t)(void* cam, void* a, void* b);
    static updatePlayer_t oUpdatePlayer = nullptr;

    static void __fastcall hkUpdatePlayer(void* cam, void* a, void* b) {
        gh::tryHookPacketSend();
        static bool s_haveFrozen = false;
        static float s_frozen[4] = { 0,0,0,0 };
        Module* fl = ModuleManager::GetInstance().GetModuleByName("Freelook");
        const bool on = fl && static_cast<Freelook*>(fl)->IsActive();
        if (on && cam) {
            float* look = reinterpret_cast<float*>((uintptr_t)cam + 0x30);
            if (!s_haveFrozen) { for (int i = 0; i < 4; ++i) s_frozen[i] = look[i]; s_haveFrozen = true; }
            float real[4]; for (int i = 0; i < 4; ++i) real[i] = look[i];
            for (int i = 0; i < 4; ++i) look[i] = s_frozen[i];
            if (oUpdatePlayer) oUpdatePlayer(cam, a, b);
            ModuleManager::GetInstance().OnUpdate();
            for (int i = 0; i < 4; ++i) look[i] = real[i];
            return;
        }
        s_haveFrozen = false;
        if (oUpdatePlayer) oUpdatePlayer(cam, a, b);
        ModuleManager::GetInstance().OnUpdate();
    }

    // Dimension::getTimeOfDay — override the INPUT day tick (0..24000), not
    // the returned celestial angle the function computes from it.
    typedef float(__fastcall* getTimeOfDay_t)(void* dim, int tick, float a3);
    static getTimeOfDay_t oGetTimeOfDay = nullptr;

    static float __fastcall hkGetTimeOfDay(void* dim, int tick, float a3) {
        gh::g_liveDimension = dim;   // owns the sole client BlockSource
        Module* tc = ModuleManager::GetInstance().GetModuleByName("TimeChanger");
        if (tc && tc->IsEnabled())
            tick = static_cast<int>(static_cast<TimeChanger*>(tc)->GetTime()) % 24000;
        return oGetTimeOfDay ? oGetTimeOfDay(dim, tick, a3) : 0.0f;
    }

    // Dimension::getSkyColor(dim, out, src, f) — out in rdx (returned), written
    // from src via movups. FogColor overrides the channels afterwards.
    typedef void*(__fastcall* getSkyColor_t)(void* dim, void* out, void* a3, float a4);
    static getSkyColor_t oGetSkyColor = nullptr;

    static void* __fastcall hkGetSkyColor(void* dim, void* out, void* a3, float a4) {
        gh::g_liveDimension = dim;
        void* res = oGetSkyColor ? oGetSkyColor(dim, out, a3, a4) : out;
        Module* fc = ModuleManager::GetInstance().GetModuleByName("FogColor");
        if (fc && fc->IsEnabled() && out) {
            FogColor* f = static_cast<FogColor*>(fc);
            float* col = reinterpret_cast<float*>(out);
            if (f->GetMode() == 1) {
                ImColor tc = ColorUtils::getThemedColor(0); // theme gradient RGB
                col[0] = tc.Value.x; col[1] = tc.Value.y; col[2] = tc.Value.z;
            } else {
                col[0] = f->GetR(); col[1] = f->GetG(); col[2] = f->GetB();
            }
        }
        return res;
    }
}

int GameHooks::LastPerspective() {
    return gh::g_lastPerspective.load(std::memory_order_relaxed);
}

// Live perspective through the REAL getter: the FreeCam ghost injection reads
// this instead of the cached mirror, so a stale value can never gate it.
int GameHooks::LivePerspectiveForGhost() {
    if (s_perspectiveOptions && oGetPerspective)
        return oGetPerspective(s_perspectiveOptions);
    return gh::g_lastPerspective.load(std::memory_order_relaxed);
}

void InstallGammaHook() {
    uintptr_t gammaAddr = SigManager::GetInstance().Fullbright;
    if (gammaAddr) {
        if (MH_CreateHook(reinterpret_cast<LPVOID>(gammaAddr), &hkOptionsGetGamma,
                           reinterpret_cast<LPVOID*>(&oOptionsGetGamma)) == MH_OK) {
            gh::HookEnable(reinterpret_cast<LPVOID>(gammaAddr));
            LOG_INFO("Options::getGamma hooked", "GameHooks");
        }
    }
}

void InstallFreelookHooks() {
    uintptr_t pAddr = SigManager::GetInstance().Options_getPerspective;
    if (pAddr && MH_CreateHook(reinterpret_cast<LPVOID>(pAddr), &hkGetPerspective,
                               reinterpret_cast<LPVOID*>(&oGetPerspective)) == MH_OK) {
        gh::HookEnable(reinterpret_cast<LPVOID>(pAddr));
        LOG_INFO("Options::getPerspective hooked", "GameHooks");
    }
    uintptr_t uAddr = SigManager::GetInstance()._updatePlayer;
    if (uAddr && MH_CreateHook(reinterpret_cast<LPVOID>(uAddr), &hkUpdatePlayer,
                               reinterpret_cast<LPVOID*>(&oUpdatePlayer)) == MH_OK) {
        gh::HookEnable(reinterpret_cast<LPVOID>(uAddr));
        LOG_INFO("_updatePlayer hooked", "GameHooks");
    }
}

void InstallWorldOverrideHooks() {
    {
        uintptr_t tAddr = SigManager::GetInstance().Dimension_getTimeOfDay;
        if (tAddr && MH_CreateHook(reinterpret_cast<LPVOID>(tAddr), &hkGetTimeOfDay,
                                   reinterpret_cast<LPVOID*>(&oGetTimeOfDay)) == MH_OK) {
            gh::HookEnable(reinterpret_cast<LPVOID>(tAddr));
            LOG_INFO("Dimension::getTimeOfDay hooked", "GameHooks");
        }
    }
    {
        uintptr_t fAddr = SigManager::GetInstance().Dimension_getSkyColor;
        if (fAddr && MH_CreateHook(reinterpret_cast<LPVOID>(fAddr), &hkGetSkyColor,
                                   reinterpret_cast<LPVOID*>(&oGetSkyColor)) == MH_OK) {
            gh::HookEnable(reinterpret_cast<LPVOID>(fAddr));
            LOG_INFO("Dimension::getSkyColor hooked", "GameHooks");
        }
    }
}
