#pragma once
// Client-side mob effects: hooks + local poll + optional listeners.
// Types/layout live in MobEffect.h; Actor::getStatusEffects() reads the component.

#include "MobEffect.h"
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace SDK {

struct EffectEvent {
    void*    actor = nullptr;
    uint32_t id = 0;
    int      amplifier = 0;
    int      durationTicks = 0;
    bool     infinite = false;
    bool     isAdd = true;
};

using EffectListener = std::function<void(const EffectEvent&)>;

class MobEffects {
public:
    static void Initialize();
    static void Shutdown();

    static std::vector<MobEffectData> getLocal();

    static void addListener(EffectListener cb);
    static void clearListeners();

    static uintptr_t addEffectAddr();
    static uintptr_t removeEffectAddr();
};

} // namespace SDK
