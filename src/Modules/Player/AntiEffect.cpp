#include "AntiEffect.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/Network/Packet.h"
#include "../../SDK/Network/PacketTypes.h"
#include "../../Memory/OffsetManager.h"
#include "../../Memory/SigManager.h"
#include <Windows.h>

// Bedrock effect ids (project MobEffect ids use the standard Bedrock enum).
namespace {
    constexpr int kSlowness       = 2;
    constexpr int kMiningFatigue  = 4;
    constexpr int kNausea         = 9;
    constexpr int kBlindness      = 15;
    constexpr int kHunger         = 17;
    constexpr int kWeakness       = 18;
    constexpr int kPoison         = 19;
    constexpr int kWither         = 20;
    constexpr int kLevitation     = 24;
    constexpr int kFatalPoison    = 25;
    constexpr int kDarkness       = 28;

    // MobEffectPacket (wire id 28, IDA-verified on 1.26.42.01).
    constexpr uintptr_t kMEP_RuntimeId = 0x30; // u64 "Actor Runtime ID"
    constexpr uintptr_t kMEP_Event     = 0x58; // u32, ctor default 1 = Add
    constexpr size_t    kMEP_Span      = 0x5C;
}

bool AntiEffect::isBlocked(int id) const {
    switch (id) {
    case kLevitation:    return GetLevitation();
    case kDarkness:      return GetDarkness();
    case kBlindness:     return GetBlindness();
    case kNausea:        return GetNausea();
    case kPoison:
    case kFatalPoison:   return GetPoison();
    case kWither:        return GetWither();
    case kHunger:        return GetHunger();
    case kSlowness:      return GetSlowness();
    case kWeakness:      return GetWeakness();
    case kMiningFatigue: return GetMiningFatigue();
    default:             return false;
    }
}

// Purge: walk the local player's MobEffectsComponent vector and neutralize
// every blocked instance. All writes are POD field pokes inside one SEH frame;
// the game's own effect ticker performs the native removal next tick.
void AntiEffect::OnUpdate() {
    if (!IsEnabled()) return;
    const int mode = GetMode();
    if (mode == 1) return; // Block All never touches the component

    SDK::ClientInstance* cInst = SDK::ClientInstance::get();
    if (!cInst) return;
    SDK::Actor* lp = cInst->getLocalPlayer();
    if (!lp) return;

    const int idOff   = Offsets().EffectInstance_id;      // always 0
    const int durOff  = Offsets().EffectInstance_duration;
    const int ampOff  = Offsets().EffectInstance_amplifier;
    const int ncOff   = Offsets().EffectInstance_noCounter;
    const int stride  = Offsets().EffectInstance_size;    // 0x88 on 1.26.x
    if (stride < 0x40 || durOff <= 0 || ampOff <= 0 || ncOff <= 0) return;

    void* comp = lp->getMobEffectsComponentRaw();
    if (!comp) return;

    __try {
        if (!sdk_safe_ptr(comp, 0x18)) return;
        const uintptr_t begin = *(uintptr_t*)comp;
        const uintptr_t end   = *(uintptr_t*)((uintptr_t)comp + 8);
        if (!begin || end < begin) return;
        const uintptr_t span = end - begin;
        if (span % (uintptr_t)stride != 0) return;      // drifted layout -> bail
        const int count = (int)(span / (uintptr_t)stride);
        if (count < 0 || count > 64) return;

        for (int i = 0; i < count; ++i) {
            const uintptr_t e = begin + (uintptr_t)i * (uintptr_t)stride;
            if (!sdk_safe_ptr((void*)e, (size_t)ncOff + 1)) return;
            const int id = *(int*)(e + idOff);
            if (!isBlocked(id)) continue;
            // Neutralize: one-tick lifetime in counter mode, no amplifier.
            // The vanilla ticker expires it and cleans up natively.
            *(int*)(e + durOff) = 1;
            *(int*)(e + ampOff) = 0;
            *(unsigned char*)(e + ncOff) = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Block All: drop MobEffectPackets that target the local player before the
// game applies anything (no one-tick flash, no HUD blink).
void AntiEffect::OnPacketReceive(SDK::Packet* packet, int packetId, bool& cancel) {
    if (!IsEnabled() || !packet || cancel) return;
    if (GetMode() == 0) return; // Purge only
    if (packetId != static_cast<int>(SDK::TypePacket::MobEffect)) return; // 28 on 1.26.42.01

    SDK::ClientInstance* cInst = SDK::ClientInstance::get();
    if (!cInst) return;
    SDK::Actor* lp = cInst->getLocalPlayer();
    if (!lp) return;

    const uint64_t selfId = lp->getRuntimeID();
    if (!selfId) return;

    __try {
        if (!sdk_safe_ptr(packet, kMEP_Span)) return;
        const uintptr_t base = reinterpret_cast<uintptr_t>(packet);
        if (*(uint64_t*)(base + kMEP_RuntimeId) != selfId) return;
        // Event 1 = Add (the ctor default). Removes/expiries stay untouched so
        // server-driven cleanup traffic keeps flowing.
        if (*(uint32_t*)(base + kMEP_Event) == 1) cancel = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
