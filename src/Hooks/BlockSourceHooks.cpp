// Live client BlockSource discovery.
//
// The BlockSource::getBlock impl detour votes for the hottest `this` — the
// winner is, by construction, exactly what the natives call thousands of
// times a second. The Dimension-embedded fallback (dim+0xF0 holder +0x20) is
// validated BEHAVIORALLY via world y-bounds: this build (1.26.60, VMP)
// relocates vtables into the heap at runtime, so every `.rdata vtable-RVA ==`
// identity check fails at runtime.
#include "GameHooks.h"
#include "HookShared.h"
#include "../SDK/SDK.h"
#include "../Core/Logger.h"
#include "../Memory/SigManager.h"
#include "MinHook.h"
#include <atomic>
#include <string>

namespace gh {
    void* g_liveDimension = nullptr;
}

namespace {
    using namespace SDK;

    typedef void* (__fastcall* tGetBlockImpl)(void* bs, const void* pos);
    static tGetBlockImpl oGetBlockImpl = nullptr;
    static std::atomic<uintptr_t> g_bsVotePtr[16] = {};
    static std::atomic<uint32_t> g_bsVoteCnt[16] = {};

    static void* __fastcall hkGetBlockImpl(void* bs, const void* pos) {
        void* r = oGetBlockImpl(bs, pos);
        const uintptr_t p = (uintptr_t)bs;
        if (p > 0x10000) {
            const unsigned i = (unsigned)((p >> 4) ^ (p >> 10)) & 15;
            uintptr_t slot = g_bsVotePtr[i].load(std::memory_order_relaxed);
            if (slot == p) {
                g_bsVoteCnt[i].fetch_add(1, std::memory_order_relaxed);
            } else if (slot == 0) {
                if (g_bsVotePtr[i].compare_exchange_strong(slot, p, std::memory_order_relaxed))
                    g_bsVoteCnt[i].store(1, std::memory_order_relaxed);
            } else {
                const uint32_t c = g_bsVoteCnt[i].load(std::memory_order_relaxed);
                if (c <= 1) {
                    g_bsVotePtr[i].store(p, std::memory_order_relaxed);
                    g_bsVoteCnt[i].store(1, std::memory_order_relaxed);
                } else {
                    g_bsVoteCnt[i].fetch_sub(1, std::memory_order_relaxed);
                }
            }
        }
        return r;
    }

    static constexpr int s_bsHolder = 0xF0;  // dim+0xF0 -> shared_ptr {holder,ctrl}
    static constexpr int s_bsEmbed   = 0x20; // holder+0x20 embeds the BS_A BlockSource

    namespace bsdiag {
        struct Facts {
            int stage = 0;                    // 1 dim, 2 holder, 3 bs fields, 5 ok
            uintptr_t dim = 0, dimVt = 0, holder = 0, bs = 0, bsVt = 0;
            int minY = 0, maxY = 0;
        };
        std::atomic<DWORD> g_lastFailLog{ 0 };
        std::atomic<bool> g_loggedOk{ false };
    }

    static SDK::BlockSource* sehGetClientBlockSource(bsdiag::Facts& f) {
        __try {
            void* dim = gh::g_liveDimension;
            f.dim = (uintptr_t)dim;
            if (!dim || !sdk_safe_ptr(dim, sizeof(void*))) return nullptr;
            f.dimVt = *(uintptr_t*)dim;
            f.stage = 1;
            if (!sdk_safe_ptr((void*)((uintptr_t)dim + s_bsHolder), sizeof(void*))) return nullptr;
            void* holder = *(void**)((uintptr_t)dim + s_bsHolder);
            f.holder = (uintptr_t)holder;
            if (!holder || !sdk_safe_ptr(holder, s_bsEmbed + 8)) return nullptr;
            f.stage = 2;
            SDK::BlockSource* bs = (SDK::BlockSource*)((uintptr_t)holder + s_bsEmbed);
            f.bs = (uintptr_t)bs;
            if (!sdk_safe_ptr(bs, 0x40)) return nullptr;
            f.bsVt = *(uintptr_t*)bs;
            f.stage = 3;
            // int16 world-y bounds — the exact fields getBlock checks y against.
            const int maxY = *(int16_t*)((uintptr_t)bs + 0x38);
            const int minY = *(int16_t*)((uintptr_t)bs + 0x3A);
            f.minY = minY; f.maxY = maxY;
            f.stage = 4;
            if (minY <= maxY && (maxY - minY) >= 16 && (maxY - minY) <= 8192 &&
                (minY % 16) == 0 && (maxY % 16) == 0 && minY >= -4096 && maxY <= 4096) {
                f.stage = 5;
                return bs;
            }
            return nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        return nullptr;
    }

    static uintptr_t bestVote(uint32_t* nOut) {
        uintptr_t best = 0; uint32_t bestN = 0;
        for (int i = 0; i < 16; ++i) {
            const uintptr_t p = g_bsVotePtr[i].load(std::memory_order_relaxed);
            const uint32_t n = g_bsVoteCnt[i].load(std::memory_order_relaxed);
            if (n > bestN && p) { bestN = n; best = p; }
        }
        if (nOut) *nOut = bestN;
        return best;
    }
}

SDK::BlockSource* GameHooks_GetClientBlockSource() {
    // 1. The getBlock vote — whichever BlockSource the natives called most
    //    recently is the authoritative live one.
    {
        uint32_t bestN = 0;
        const uintptr_t best = bestVote(&bestN);
        if (best && bestN >= 64 && sdk_safe_ptr((void*)best, 0x40)) {
            SDK::BlockSource* w = (SDK::BlockSource*)best;
#ifdef SUNSTEAD_LOGGING
            static std::atomic<bool> s_loggedVote{ false };
            if (!s_loggedVote.exchange(true, std::memory_order_relaxed))
                LOG_INFO("blocksource vote ok bs=" + std::to_string(best) +
                         " votes=" + std::to_string(bestN) +
                         " y=" + std::to_string(*(int16_t*)((uintptr_t)best + 0x3A)) +
                         "/" + std::to_string(*(int16_t*)((uintptr_t)best + 0x38)), "BlockSource");
#endif
            return w;
        }
    }
    bsdiag::Facts f;
    SDK::BlockSource* bs = sehGetClientBlockSource(f);
#ifdef SUNSTEAD_LOGGING
    if (bs) {
        if (!bsdiag::g_loggedOk.exchange(true, std::memory_order_relaxed))
            LOG_INFO("blocksource ok dim=" + std::to_string(f.dim) +
                     " bs=" + std::to_string(f.bs) +
                     " y[" + std::to_string(f.minY) + "," + std::to_string(f.maxY) + ")", "BlockSource");
        return bs;
    }
    const DWORD now = GetTickCount();
    const DWORD last = bsdiag::g_lastFailLog.load(std::memory_order_relaxed);
    if (now - last < 15000) return nullptr;
    bsdiag::g_lastFailLog.store(now, std::memory_order_relaxed);
    {
        uint32_t bestN = 0;
        const uintptr_t best = bestVote(&bestN);
        LOG_INFO("no blocksource: stage=" + std::to_string(f.stage) +
                 " dim=" + std::to_string(f.dim) +
                 " dimVt=" + std::to_string(f.dimVt) +
                 " holder=" + std::to_string(f.holder) +
                 " bs=" + std::to_string(f.bs) +
                 " bsVt=" + std::to_string(f.bsVt) +
                 " y=" + std::to_string(f.minY) + "/" + std::to_string(f.maxY) +
                 " vote=" + std::to_string(best) + ":" + std::to_string(bestN), "BlockSource");
    }
#endif
    return bs;
}

void InstallBlockSourceHooks() {
    uintptr_t getBlockAddr = SigManager::GetInstance().BlockSource_getBlock;
    if (getBlockAddr) {
        if (MH_CreateHook(reinterpret_cast<LPVOID>(getBlockAddr), &hkGetBlockImpl,
                          reinterpret_cast<LPVOID*>(&oGetBlockImpl)) == MH_OK) {
            gh::HookEnable(reinterpret_cast<LPVOID>(getBlockAddr));
            LOG_INFO("getBlock capture hooked", "GameHooks");
        } else {
            LOG_ERROR("FAILED to hook getBlock impl", "GameHooks");
        }
    } else {
        LOG_WARN("BlockSource::getBlock signature not resolved", "GameHooks");
    }
}
