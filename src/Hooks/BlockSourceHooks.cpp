#include "GameHooks.h"
#include "HookShared.h"
#include "../SDK/SDK.h"
#include "../Core/Logger.h"
#include "../Memory/SigManager.h"
#include "HookEngine.h"
#include <atomic>
#include <string>

namespace gh {
    void* g_liveDimension = nullptr;
}

namespace {
    using namespace SDK;

    typedef void* (__fastcall* tGetBlockImpl)(void* bs, const void* pos);
    static tGetBlockImpl oGetBlockImpl = nullptr;
    static std::atomic<uintptr_t> g_blockSourceVtable = 0;

    // ---------------------------------------------------------------------------
    // THREAD OWNERSHIP (crash-20261110-011107 / -030242 / -094849, three
    // identical dumps)
    //
    // BlockSource stores the id of the thread it was constructed on at +0x18
    // (BlockSource ctor: "*(DWORD*)(this + 0x18) = Thrd_id()", 1.26.6x). The
    // last-chunk cache in BlockSource (fields +0xB8 key / +0xC0 / +0xC8 ctrl /
    // +0xD0 raw, maintained by getChunk = BlockSource vtable slot 41,
    // sub_1432CD4B0) is NOT thread-safe: every cache swap does non-atomic
    // shared/weak control-block inc/dec traffic. Using a BlockSource from any
    // thread other than its owner double-releases a cached ctrl block; the
    // game then crashes later inside its OWN getChunk releasing the stale
    // cache entry (AV read at 0x8 through a nulled ctrl vtable) on whichever
    // thread touches the poisoned instance next — usually the render-side
    // block-actor neighborhood walk ("Player - Pre render update").
    //
    // The old capture promoted ONE leader out of ALL getBlock traffic,
    // including the singleplayer server thread's own BlockSource (server ticks
    // hammer getBlock harder than the client does). Our modules then used
    // that instance from the main thread — the exact race above.
    //
    // Fix: capture per THREAD (each thread only votes for / uses instances it
    // owns by +0x18), and never serve an instance whose owner thread is not
    // the calling thread.
    // ---------------------------------------------------------------------------

    struct ThreadLeader {
        std::atomic<uint32_t> tid;
        std::atomic<uintptr_t> bs;
        std::atomic<int> score;
    };
    static ThreadLeader g_leaders[16]{};
    constexpr int kPromote = 4;
    constexpr int kScoreCap = 64;

    static bool isOnThreadStack(void* p) {
#ifdef _WIN64
        const uintptr_t base = __readgsqword(0x08);
        const uintptr_t limit = __readgsqword(0x10);
#else
        const uintptr_t base = __readgsdword(0x04);
        const uintptr_t limit = __readgsdword(0x08);
#endif
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v >= limit && v < base;
    }

    static uint32_t myTid() {
#ifdef _WIN64
        return (uint32_t)__readgsqword(0x48);  // TEB ClientId.UniqueThread
#else
        return __readgsdword(0x24);
#endif
    }

    static bool isValidHeightRange(void* bs) {
        __try {
            const int16_t lo = *reinterpret_cast<int16_t*>(reinterpret_cast<uintptr_t>(bs) + 0x3A);
            const int16_t hi = *reinterpret_cast<int16_t*>(reinterpret_cast<uintptr_t>(bs) + 0x38);
            return lo <= hi && (lo != 0 || hi != 0) &&
                   lo > -16384 && hi < 16384;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Owning thread of a BlockSource instance; 0 on any fault.
    static uint32_t bsOwnerTid(void* bs) {
        __try {
            return *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(bs) + 0x18);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    static ThreadLeader* leaderClaim() {
        const uint32_t tid = myTid();
        ThreadLeader* steal = nullptr;
        int stealScore = 0x40000000;
        for (auto& l : g_leaders) {
            uint32_t t = l.tid.load(std::memory_order_acquire);
            if (t == tid) return &l;
            if (t == 0) {
                if (l.tid.compare_exchange_strong(t, tid)) {
                    l.bs.store(0, std::memory_order_relaxed);
                    l.score.store(0, std::memory_order_relaxed);
                    return &l;
                }
                continue;
            }
            const int s = l.score.load(std::memory_order_acquire);
            if (s < stealScore) { stealScore = s; steal = &l; }
        }
        if (!steal) return nullptr;
        // All slots claimed by other threads: take over the least active one.
        uint32_t t = steal->tid.load(std::memory_order_acquire);
        if (steal->tid.compare_exchange_strong(t, tid)) {
            steal->bs.store(0, std::memory_order_relaxed);
            steal->score.store(0, std::memory_order_relaxed);
            return steal;
        }
        return nullptr;
    }

    static void* __fastcall hkGetBlockImpl(void* bs, const void* pos) {
        if (bs && !isOnThreadStack(bs) && sdk_safe_ptr(bs, 0x40) &&
            isValidHeightRange(bs) && bsOwnerTid(bs) == myTid()) {
            if (g_blockSourceVtable.load(std::memory_order_relaxed) == 0)
                g_blockSourceVtable.store(*reinterpret_cast<uintptr_t*>(bs), std::memory_order_relaxed);
            ThreadLeader* l = leaderClaim();
            if (l) {
                const uintptr_t v = reinterpret_cast<uintptr_t>(bs);
                if (l->bs.load(std::memory_order_relaxed) == v) {
                    int score = l->score.load(std::memory_order_relaxed);
                    if (score < kScoreCap)
                        l->score.store(score + kPromote, std::memory_order_relaxed);
                } else {
                    int score = l->score.fetch_sub(1, std::memory_order_relaxed) - 1;
                    if (score <= 0) {
                        l->bs.store(v, std::memory_order_relaxed);
                        l->score.store(kPromote, std::memory_order_relaxed);
                    }
                }
            }
        }
        return oGetBlockImpl ? oGetBlockImpl(bs, pos) : nullptr;
    }
}

SDK::BlockSource* GameHooks_GetClientBlockSource() {
    ThreadLeader* l = leaderClaim();
    if (!l) return nullptr;
    auto* cached = reinterpret_cast<SDK::BlockSource*>(l->bs.load(std::memory_order_acquire));
    __try {
        if (!cached || !sdk_safe_ptr(cached, 0x40)) return nullptr;
        if (isOnThreadStack(cached)) return nullptr;
        if (bsOwnerTid(cached) != myTid()) return nullptr;  // never cross-thread
        const uintptr_t vt = g_blockSourceVtable.load(std::memory_order_acquire);
        if (vt && *reinterpret_cast<uintptr_t*>(cached) != vt) return nullptr;
        if (isValidHeightRange(cached)) return cached;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

void InstallBlockSourceHooks() {
    uintptr_t getBlockAddr = SigManager::GetInstance().BlockSource_getBlock;
    if (getBlockAddr) {
        if (HookEngine::Create(reinterpret_cast<LPVOID>(getBlockAddr), &hkGetBlockImpl,
                               reinterpret_cast<LPVOID*>(&oGetBlockImpl))) {
            LOG_INFO("getBlock capture hooked", "GameHooks");
        } else {
            LOG_ERROR("FAILED to hook getBlock impl", "GameHooks");
        }
    } else {
        LOG_WARN("BlockSource::getBlock signature not resolved", "GameHooks");
    }
}
