#include "TrueSight.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/Actor/MobEffect.h"
#include <vector>
#include <unordered_set>
#include <Windows.h>

// ===========================================================================
// TrueSight (see header). Every SDK accessor used here is internally
// SEH-guarded (Actor::getStatusFlag / setStatusFlag / getStatusEffects are
// POD-only probes), so plain calls suffice; the one unguarded native call
// (Level::getRuntimeActorListInto, which fills a std::vector) lives in its own
// object-free helper so __try stays legal (MSVC C2712).
//
// While enabled, every actor whose Invisible flag we clear is remembered by
// its UNIQUE ID — pointers dangle, IDs are stable — and OnDisable walks the
// live actor list once more and re-arms the flag on every one of them, so
// NOTHING stays visible after the toggle (the old effect-only restore left
// server-flagged actors visible forever).
// ===========================================================================

namespace {

bool tsResolve(SDK::Level** level, SDK::Actor** lp) {
    __try {
        SDK::ClientInstance* ci = SDK::ClientInstance::get();
        if (!ci || !sdk_safe_ptr(ci, 0x200)) return false;
        SDK::Minecraft* mc = ci->getMinecraft();
        if (!mc || !sdk_safe_ptr(mc, 0xC0)) return false;
        *level = mc->getLevel();
        if (!*level || !sdk_safe_ptr(*level, 0x10)) return false;
        *lp = ci->getLocalPlayer();
        if (!*lp || !sdk_safe_ptr(*lp, 0x230)) return false;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool tsGetActors(SDK::Level* level, std::vector<SDK::Actor*>& out) {
    __try { level->getRuntimeActorListInto(&out); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Actors we cleared, by unique ID (restored on disable).
std::unordered_set<uint64_t> g_clearedIds;

// True when the actor carries the Invisibility mob effect. getStatusEffects
// is SEH-guarded internally (empty list on any fault).
bool tsHasInvisibilityEffect(SDK::Actor* a) {
    std::vector<SDK::MobEffectData> fx = a->getStatusEffects();
    for (const auto& e : fx)
        if (e.id == (uint32_t)SDK::EffectType::Invisibility) return true;
    return false;
}

uint64_t tsUniqueId(SDK::Actor* a) {
    __try { return a->getUniqueID(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

} // namespace

void TrueSight::OnTick() {
    SDK::Level* level = nullptr; SDK::Actor* lp = nullptr;
    if (!tsResolve(&level, &lp)) return;
    std::vector<SDK::Actor*> actors;
    if (!tsGetActors(level, actors)) return;

    for (SDK::Actor* a : actors) {
        if (!a || a == lp || !sdk_safe_ptr(a, 0x230)) continue;
        // Clear the render-side flags. The LocalPlayer is skipped on purpose:
        // writing its synced flags could leak into outgoing packet state.
        bool cleared = false;
        if (a->getStatusFlag(ActorFlags::Invisible) == 1) {
            a->setStatusFlag(ActorFlags::Invisible, false);
            cleared = true;
        }
        if (a->getStatusFlag(ActorFlags::HiddenWhenInvisible) == 1)
            a->setStatusFlag(ActorFlags::HiddenWhenInvisible, false);
        if (cleared) {
            const uint64_t id = tsUniqueId(a);
            if (id) g_clearedIds.insert(id);
        }
    }
}

void TrueSight::OnDisable() {
    // Restore EVERY actor we cleared: re-raise the flags on the live actors
    // whose unique ID is in the cleared set. Actors that despawned in the
    // meantime need nothing (the server re-syncs them); the set dies with the
    // restore so a fresh enable starts from scratch.
    SDK::Level* level = nullptr; SDK::Actor* lp = nullptr;
    if (tsResolve(&level, &lp)) {
        std::vector<SDK::Actor*> actors;
        if (tsGetActors(level, actors)) {
            for (SDK::Actor* a : actors) {
                if (!a || a == lp || !sdk_safe_ptr(a, 0x230)) continue;
                if (g_clearedIds.count(tsUniqueId(a)) ||
                    tsHasInvisibilityEffect(a)) {
                    a->setStatusFlag(ActorFlags::Invisible, true);
                    a->setStatusFlag(ActorFlags::HiddenWhenInvisible, true);
                }
            }
        }
    }
    g_clearedIds.clear();
}

