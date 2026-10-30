#include "FastEat.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/SDK.h"
#include "../../Core/Logger.h"
#include <cstdint>
#include <Windows.h>

namespace {

// FNV-1a 32 of "ItemInUseComponent" — verified against the descriptor hash the
// game itself compares in Mob::getItemUseDuration / startUsingItem on 1.26.50.04.
constexpr uint32_t kItemInUseHash = 0x5B90AEFu;

// Byte-exact replication of the game's own ItemInUseComponent access
// (sub_1401EDE80, Mob::getItemUseDuration):
//
//   reg = *(Actor+0x10)          entt component-storage registry
//   ent = *(uint32*)(Actor+0x18) entity id (Actor+0x8 is the EntityContext)
//
//   storage lookup — registry hash map:
//     buckets:  vector @ reg+0x48, power-of-two bucket count
//     chain:    bucket[hash & mask] -> descriptor[0] -> ... until -1
//     desc:     32 bytes { u64 nextIdx; u32 hash @ +8; storage* @ +0x10 }
//     reg+0x70: end-sentinel descriptor (must not be used)
//
//   element lookup — entt sparse set on the storage:
//     page = (ent & 0x3FFFF) >> 11, slot = ent & 0x7FF
//     idx  = sparsePages[page][slot]; present when (idx ^ ent&0xFFFC0000) <= 0x3FFFE
//     elem = payloadPages[(idx & 0x3FF80) >> 4] + 16 * (idx & 0x7F)
//     remainingTicks = *(int32*)(elem + 8)
//
// Returns a pointer to the remaining-ticks int, or nullptr when the player is
// not using an item / anything in the chain is unreadable. POD-only + SEH.
int* sehInUseDuration(SDK::Actor* lp) {
    __try {
        if (!lp || !sdk_safe_ptr(lp, 0x20)) return nullptr;

        const uintptr_t ctx = (uintptr_t)lp + 0x8;         // EntityContext
        if (!sdk_safe_ptr((void*)ctx, 0x18)) return nullptr;
        const uintptr_t reg = *(uintptr_t*)(ctx + 0x8);    // == *(Actor+0x10)
        const uint32_t  ent = *(uint32_t*)(ctx + 0x10);    // == *(Actor+0x18)
        if (!reg || !sdk_safe_ptr((void*)reg, 0x78)) return nullptr;

        // ---- registry descriptor chain --------------------------------------
        const uintptr_t bBegin = *(uintptr_t*)(reg + 0x48);
        const uintptr_t bEnd   = *(uintptr_t*)(reg + 0x50);
        if (!bBegin || bEnd <= bBegin || (bEnd - bBegin) < 8) return nullptr;
        const uint32_t mask = (uint32_t)((bEnd - bBegin) >> 3) - 1;
        if (mask == 0xFFFFFFFFu) return nullptr;

        const uintptr_t descBase = *(uintptr_t*)(reg + 0x68);
        if (!descBase || !sdk_safe_ptr((void*)descBase, 0x20)) return nullptr;
        const uintptr_t endDesc = *(uintptr_t*)(reg + 0x70);

        uintptr_t node = *(uintptr_t*)(bBegin + 8ull * (kItemInUseHash & mask));
        uintptr_t storage = 0;
        int hops = 0;
        while (node != 0xFFFFFFFFFFFFFFFFull && hops++ < 64) {
            const uintptr_t d = descBase + 32ull * node;
            if (!sdk_safe_ptr((void*)d, 0x20)) return nullptr;
            if (*(uint32_t*)(d + 8) == kItemInUseHash) {
                if (d == endDesc) return nullptr;   // the end-sentinel descriptor
                storage = *(uintptr_t*)(d + 0x10);
                break;
            }
            node = *(uintptr_t*)d;
        }
        if (!storage || !sdk_safe_ptr((void*)storage, 0x58)) return nullptr;

        // ---- entt sparse set --------------------------------------------------
        const uint32_t page = (ent & 0x3FFFFu) >> 11;
        const uintptr_t sBegin = *(uintptr_t*)(storage + 0x08);
        const uintptr_t sEnd   = *(uintptr_t*)(storage + 0x10);
        if (!sBegin || sEnd <= sBegin) return nullptr;
        if ((size_t)page >= (size_t)((sEnd - sBegin) >> 3)) return nullptr;
        const uintptr_t pg = *(uintptr_t*)(sBegin + 8ull * page);
        if (!pg || !sdk_safe_ptr((void*)pg, 4ull * 0x800)) return nullptr;

        const uint32_t idx = *(uint32_t*)(pg + 4ull * (ent & 0x7FF));
        if ((idx ^ (ent & 0xFFFC0000u)) > 0x3FFFEu) return nullptr;   // not in the set

        const uintptr_t pages = *(uintptr_t*)(storage + 0x50);
        if (!pages || !sdk_safe_ptr((void*)pages, sizeof(void*))) return nullptr;
        const uintptr_t chunk = *(uintptr_t*)(pages + ((idx & 0x3FF80u) >> 4));
        if (!chunk || !sdk_safe_ptr((void*)chunk, 0x100)) return nullptr;

        int* dur = (int*)(chunk + 16ull * (idx & 0x7F) + 8);
        return sdk_safe_ptr(dur, 4) ? dur : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

} // namespace

void FastEat::OnUpdate() {
    if (!IsEnabled()) return;

    const float speed = static_cast<NumberSetting*>(mSettings[0])->mValue;
    if (speed <= 1.0f) return;                        // vanilla
    const bool dbg = mSettings.size() > 1 && static_cast<BoolSetting*>(mSettings[1])->mValue;

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    SDK::Actor* lp = ci ? ci->getLocalPlayer() : nullptr;
    if (!lp || !sdk_safe_ptr(lp, 0x20)) return;

    // OnUpdate runs per camera frame; burn once per GAME tick so Speed is a
    // true "times faster" multiplier regardless of framerate.
    const long long tick = lp->getTicksExisted();
    if (tick >= 0) {
        if (tick == mLastTick) return;
        mLastTick = tick;
    } else {
        // Ticks unreadable — fall back to a ~50 ms wall-clock gate.
        static unsigned long long sLast = 0;
        const unsigned long long now = GetTickCount64();
        if (now - sLast < 50) return;
        sLast = now;
    }

    int* dur = sehInUseDuration(lp);
    if (!dur) return;                                 // not using an item right now

    const int v = *dur;
    if (v <= 0 || v > 72000) return;                   // sanity: < 1 hour of ticks

    // Fractional burn: a speed of 1.75 removes 0.75 extra ticks per game tick;
    // the whole part is applied now and the remainder carried to the next tick.
    mBurnCarry += speed - 1.0f;
    int burn = (int)mBurnCarry;
    mBurnCarry -= (float)burn;
    if (burn < 1) return;
    int nv = v - burn;
    if (nv < 1) nv = 1;                               // let the game's own finish path run
    *dur = nv;

    if (dbg)
        LOG_INFO("counter " + std::to_string(v) + " -> " + std::to_string(nv), "FastEat");
}

void FastEat::OnEnable()  { mLastTick = -1; mBurnCarry = 0.0f; }
void FastEat::OnDisable() { mLastTick = -1; mBurnCarry = 0.0f; }
