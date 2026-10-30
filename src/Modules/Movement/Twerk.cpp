#include "Twerk.h"
#include "../../Utils/MCSDK.h"
#include "../Setting.h"
#include <cstring>

// todo change it
static constexpr uint32_t kMoveInputHash     = 0x018B1887u;  // MoveInputComponent
static constexpr uint32_t kMoveInputHashAlt  = 0x468BCD2Eu;  // legacy fallback
static constexpr uint32_t kRawMoveInputHash  = 0x3617B6D3u;  // RawMoveInputComponent

std::atomic<bool> Twerk::s_wantSneak{ false };
std::atomic<int>  Twerk::s_edge{ 0 };

bool Twerk::WantSneak() { return s_wantSneak.load(std::memory_order_relaxed); }

bool Twerk::ConsumeEdge(bool& start, bool& stop) {
    int e = s_edge.exchange(0, std::memory_order_acq_rel);
    start = (e == 1);
    stop  = (e == 2);
    return start || stop;
}

struct MoveInputComponent { unsigned char b[0x64]; };

constexpr uintptr_t kFlagsBase = 0x10;

static bool sehWriteHash(SDK::Actor* lp, uint32_t hash, bool down) {
    if (!lp || !g_entt_assure) return false;
    __try {
        auto* ctx = (SDK::EntityContext*)((uintptr_t)lp + 0x8);
        if (!ctx || !sdk_safe_ptr(ctx, sizeof(SDK::EntityContext))) return false;
        void* reg = *(void**)((uintptr_t)ctx + 8);
        if (!reg) return false;
        void* sp = entt_assure_seh(g_entt_assure, reg, hash);
        if (!sp) return false;
        MoveInputComponent* mi = SDK::enttGetComp<MoveInputComponent>(sp, ctx->entity);
        if (!mi || !sdk_safe_ptr(mi, sizeof(MoveInputComponent))) return false;

        const unsigned char v = down ? 1 : 0;

        // Flag bitset at +0x10 (first word covers flags 0..31).
        auto* flags = (uint32_t*)(mi->b + kFlagsBase);
        if (down) *flags |= 0x01u;
        else      *flags &= ~0x01u;

        mi->b[0x00] = down ? (unsigned char)(mi->b[0x00] | 0x01)
                           : (unsigned char)(mi->b[0x00] & ~0x01);
        mi->b[0x20] = v;
        mi->b[0x28] = v;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool Twerk::applyMoveInputSneak(bool down) {
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci || !sdk_safe_ptr(ci, 0x200)) return false;
    SDK::Actor* lp = ci->getLocalPlayer();
    if (!lp || !sdk_safe_ptr(lp, 0x230)) return false;

    bool ok = sehWriteHash(lp, kMoveInputHash, down);
    ok = sehWriteHash(lp, kMoveInputHashAlt, down) || ok;
    ok = sehWriteHash(lp, kRawMoveInputHash, down) || ok;
    return ok;
}

void Twerk::OnEnable() {
    mSneaking = false;
    mPrevSneaking = false;
    mLastToggleMs = 0;
    s_wantSneak.store(false, std::memory_order_relaxed);
    s_edge.store(0, std::memory_order_relaxed);
}

void Twerk::OnDisable() {
    if (mSneaking) s_edge.store(2, std::memory_order_relaxed); // STOP
    s_wantSneak.store(false, std::memory_order_relaxed);
    applyMoveInputSneak(false);
    mSneaking = false;
    mPrevSneaking = false;
    mLastToggleMs = 0;
}

void Twerk::OnTick() {
    if (!IsEnabled()) return;

    const unsigned long long now = GetTickCount64();
    const unsigned long long delay = (unsigned long long)(GetDelay() > 0.f ? GetDelay() : 1.f);
    if (mLastToggleMs == 0 || (now - mLastToggleMs) >= delay) {
        mSneaking = !mSneaking;
        mLastToggleMs = now;
        if (mSneaking && !mPrevSneaking) s_edge.store(1, std::memory_order_relaxed); // START
        if (!mSneaking && mPrevSneaking) s_edge.store(2, std::memory_order_relaxed); // STOP
        mPrevSneaking = mSneaking;
    }

    s_wantSneak.store(mSneaking, std::memory_order_relaxed);
    applyMoveInputSneak(mSneaking);
}

void Twerk::OnUpdate() {
    if (!IsEnabled()) return;
    s_wantSneak.store(mSneaking, std::memory_order_relaxed);
    applyMoveInputSneak(mSneaking);
}
