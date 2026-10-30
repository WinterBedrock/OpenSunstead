#include "MobEffects.h"
#include "Actor.h"
#include "../Client/ClientInstance.h"
#include "../../Memory/SigManager.h"
#include "../../Memory/OffsetManager.h"
#include "../../Core/Logger.h"
#include "MinHook.h"

#include <atomic>

namespace SDK {
namespace {

std::mutex g_listenerMu;
std::vector<EffectListener> g_listeners;
std::atomic<bool> g_inited{ false };

using AddFn = uint64_t(__fastcall*)(void* actor, void* effectInstance);
using RemFn = int64_t(__fastcall*)(void* actor, unsigned int effectId);
AddFn oAdd = nullptr;
RemFn oRem = nullptr;

struct InstFields {
    uint32_t id;
    int32_t duration;
    int32_t amplifier;
    unsigned char noCounter;
};

bool sehReadInst(const void* inst, InstFields* out) {
    if (!inst || !out) return false;
    const int offDur = Offsets().EffectInstance_duration;
    const int offAmp = Offsets().EffectInstance_amplifier;
    const int offNc  = Offsets().EffectInstance_noCounter;
    if (offDur <= 0 || offAmp <= 0 || offNc <= 0) return false;
    __try {
        if (!sdk_safe_ptr((void*)inst, (size_t)offNc + 1)) return false;
        auto* p = reinterpret_cast<const unsigned char*>(inst);
        out->id        = *reinterpret_cast<const uint32_t*>(p + 0x00);
        out->duration  = *reinterpret_cast<const int32_t*>(p + offDur);
        out->amplifier = *reinterpret_cast<const int32_t*>(p + offAmp);
        out->noCounter = *reinterpret_cast<const unsigned char*>(p + offNc);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void fire(const EffectEvent& ev) {
    std::vector<EffectListener> copy;
    {
        std::lock_guard<std::mutex> lock(g_listenerMu);
        copy = g_listeners;
    }
    for (auto& cb : copy)
        if (cb) cb(ev);
}

uint64_t __fastcall hkAdd(void* actor, void* inst) {
    InstFields f{};
    if (sehReadInst(inst, &f) && f.id > 0 && f.id <= 36) {
        EffectEvent ev;
        ev.actor = actor;
        ev.id = f.id;
        ev.amplifier = f.amplifier;
        ev.durationTicks = f.duration;
        ev.infinite = (f.noCounter != 0 || f.duration <= 0);
        ev.isAdd = true;
        fire(ev);
    }
    return oAdd ? oAdd(actor, inst) : 0;
}

int64_t __fastcall hkRem(void* actor, unsigned int effectId) {
    if (effectId > 0 && effectId <= 36) {
        EffectEvent ev;
        ev.actor = actor;
        ev.id = effectId;
        ev.isAdd = false;
        fire(ev);
    }
    return oRem ? oRem(actor, effectId) : 0;
}

} // namespace

void MobEffects::Initialize() {
    if (g_inited.exchange(true)) return;

    auto& sig = SigManager::GetInstance();
    if (sig.Actor_addEffect) {
        if (MH_CreateHook(reinterpret_cast<LPVOID>(sig.Actor_addEffect),
                          &hkAdd, reinterpret_cast<LPVOID*>(&oAdd)) == MH_OK) {
            MH_EnableHook(reinterpret_cast<LPVOID>(sig.Actor_addEffect));
            LOG_INFO("Actor::addEffect hooked", "MobEffects");
        } else {
            LOG_ERROR("failed to hook Actor::addEffect", "MobEffects");
        }
    } else {
        LOG_WARN("Actor::addEffect sig not found", "MobEffects");
    }

    if (sig.Actor_removeEffect) {
        if (MH_CreateHook(reinterpret_cast<LPVOID>(sig.Actor_removeEffect),
                          &hkRem, reinterpret_cast<LPVOID*>(&oRem)) == MH_OK) {
            MH_EnableHook(reinterpret_cast<LPVOID>(sig.Actor_removeEffect));
            LOG_INFO("Actor::removeEffect hooked", "MobEffects");
        } else {
            LOG_ERROR("failed to hook Actor::removeEffect", "MobEffects");
        }
    } else {
        LOG_WARN("Actor::removeEffect sig not found", "MobEffects");
    }
}

void MobEffects::Shutdown() {
    clearListeners();
    g_inited = false;
}

std::vector<MobEffectData> MobEffects::getLocal() {
    auto* ci = ClientInstance::get();
    if (!ci) return {};
    auto* lp = ci->getLocalPlayer();
    if (!lp || !sdk_safe_ptr(lp, 0x20)) return {};
    return lp->getStatusEffects();
}

void MobEffects::addListener(EffectListener cb) {
    std::lock_guard<std::mutex> lock(g_listenerMu);
    g_listeners.push_back(std::move(cb));
}

void MobEffects::clearListeners() {
    std::lock_guard<std::mutex> lock(g_listenerMu);
    g_listeners.clear();
}

uintptr_t MobEffects::addEffectAddr() {
    return SigManager::GetInstance().Actor_addEffect;
}

uintptr_t MobEffects::removeEffectAddr() {
    return SigManager::GetInstance().Actor_removeEffect;
}

} // namespace SDK
