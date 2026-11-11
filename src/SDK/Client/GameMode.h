#pragma once

#include "../Types.h"
#include "../Actor/Actor.h"
#include "ClientInstance.h"
#include "../World/World.h"
#include "../../Memory/OffsetManager.h"
#include "../../Memory/SigManager.h"
#include <cstdint>
#include <vector>

// GameMode::getDestroyRate — published by SDKRuntime::Publish().
extern uintptr_t g_GameMode_getDestroyRate;
extern uintptr_t g_GameMode_buildBlock;

namespace SDK {

// Integer block position — MSVC passes `glm::ivec3 const&` as a pointer in rdx.
struct IVec3 {
    int x = 0, y = 0, z = 0;
    IVec3() = default;
    IVec3(int x_, int y_, int z_) : x(x_), y(y_), z(z_) {}
    explicit IVec3(const BlockPos& p) : x(p.x), y(p.y), z(p.z) {}
};

namespace GameMode {

constexpr int kStartDestroy    = ::Layout::VTable::GameMode_startDestroy;
constexpr int kDestroyBlock    = ::Layout::VTable::GameMode_destroyBlock;
constexpr int kContinueDestroy = ::Layout::VTable::GameMode_continueDestroy;
constexpr int kStopDestroy     = ::Layout::VTable::GameMode_stopDestroy;
constexpr int kBuildBlock      = ::Layout::VTable::GameMode_buildBlock;
constexpr int kGetPickRange    = ::Layout::VTable::GameMode_getPickRange;
constexpr int kUseItem         = ::Layout::VTable::GameMode_useItem;

extern bool g_GameMode_SkipBuildCooldown;
constexpr int kAttack          = ::Layout::VTable::GameMode_attack;
constexpr uintptr_t kPlayerOff = ::Layout::GameMode::PlayerBackRef;

inline bool isValid(void* gm, Actor* lp, void*** vtOut, bool requirePlayerRef = true) {
    if (vtOut) *vtOut = nullptr;
    if (!gm || !lp || !vtOut || !sdk_safe_ptr(gm, kPlayerOff + sizeof(void*))) return false;
    __try {
        void** vt = *reinterpret_cast<void***>(gm);
        if (!vt || !sdk_safe_ptr(vt, sizeof(void*))) return false;
        if (requirePlayerRef && *reinterpret_cast<Actor**>(
            reinterpret_cast<uintptr_t>(gm) + kPlayerOff) != lp) return false;
        *vtOut = vt;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline std::atomic<void*>& activeGameMode() {
    static std::atomic<void*> instance{ nullptr };
    return instance;
}

inline bool remember(void* gm) {
    if (activeGameMode().load(std::memory_order_acquire) == gm) return true;
    auto* ci = ClientInstance::get();
    Actor* lp = ci ? ci->getLocalPlayer() : nullptr;
    void** vt = nullptr;
    if (!isValid(gm, lp, &vt)) return false;
    activeGameMode().store(gm, std::memory_order_release);
    return true;
}

inline void* resolve(Actor* lp, void*** vtOut) {
    void* gm = activeGameMode().load(std::memory_order_acquire);
    return isValid(gm, lp, vtOut) ? gm : nullptr;
}

// Mining pipeline

inline bool startDestroy(void* gm, void** vt, const IVec3& pos, int face) {
    if (!gm || !vt) return false;
    __try {
        void* fn = vt[kStartDestroy];
        if (!fn) return false;
        bool destroyed = false;
        using fn_t = void(__fastcall*)(void*, const IVec3*, int, bool*);
        ((fn_t)fn)(gm, &pos, face, &destroyed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline bool continueDestroy(void* gm, void** vt, const IVec3& pos, int face,
                            const Vec3* playerPos, bool* destroyedOut) {
    if (!gm || !vt || !playerPos) return false;
    __try {
        void* fn = vt[kContinueDestroy];
        if (!fn) return false;
        bool destroyed = false;
        using fn_t = void(__fastcall*)(void*, const IVec3*, int, const Vec3*, bool*);
        ((fn_t)fn)(gm, &pos, face, playerPos, &destroyed);
        if (destroyedOut) *destroyedOut = destroyed;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline bool destroyBlock(void* gm, void** vt, const IVec3& pos, int face) {
    if (!gm || !vt) return false;
    __try {
        void* fn = vt[kDestroyBlock];
        if (!fn) return false;
        using fn_t = void(__fastcall*)(void*, const IVec3*, int);
        ((fn_t)fn)(gm, &pos, face);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline bool stopDestroy(void* gm, void** vt, const IVec3& pos) {
    if (!gm || !vt) return false;
    __try {
        void* fn = vt[kStopDestroy];
        if (!fn) return false;
        using fn_t = void(__fastcall*)(void*, const IVec3*);
        ((fn_t)fn)(gm, &pos);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Vanilla break timing

inline float getDestroyRate(void* gm, Block* block) {
    if (!gm || !block || !g_GameMode_getDestroyRate) return 0.f;
    if (!sdk_safe_ptr(gm, 0x10) || !sdk_safe_ptr(block, 0x10)) return 0.f;
    if (!sdk_safe_ptr((void*)g_GameMode_getDestroyRate, 16)) return 0.f;
    __try {
        using fn_t = float(__fastcall*)(void*, Block*);
        const float r = ((fn_t)g_GameMode_getDestroyRate)(gm, block);
        // Reject NaN / negatives / absurd values rather than letting them poison
        // the caller's accumulator.
        if (!(r > 0.f) || r > 1000.f) return 0.f;
        return r;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0.f; }
}

inline bool useItemDirect(void* gm, void** vt, ItemStack* held) {
    if (!gm || !held) return false;
    if (!sdk_safe_ptr(gm, 0x10) || !sdk_safe_ptr(held, ::Layout::Probe::ItemStack)) return false;

    void* fn = (void*)SigManager::GetInstance().GameMode_useItem;
    if (!fn || !sdk_safe_ptr(fn, 16)) {
        __try {
            if (vt && sdk_safe_ptr(vt, ::Layout::VTable::Guard(kUseItem + 1))) {
                fn = vt[kUseItem];
                if (fn && !sdk_safe_ptr(fn, 16)) fn = nullptr;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { fn = nullptr; }
    }
    if (!fn) return false;

    void* player = nullptr;
    if (sdk_safe_ptr(gm, kPlayerOff + sizeof(void*)))
        player = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(gm) + kPlayerOff);

    __try {
        using fn_t = bool(__fastcall*)(void*, ItemStack*, void*, std::uint8_t);
        return ((fn_t)fn)(gm, held, player, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline bool buildBlock(void* gm, void** vt, const IVec3& pos, unsigned char face) {
    if (!gm) return false;
    if (g_GameMode_SkipBuildCooldown && sdk_safe_ptr(gm, ::Layout::GameMode::BuildCooldownLastNs + 8))
        *reinterpret_cast<std::uint64_t*>(reinterpret_cast<std::uintptr_t>(gm)
                                          + ::Layout::GameMode::BuildCooldownLastNs) = 0;
    __try {
        void* fn = reinterpret_cast<void*>(g_GameMode_buildBlock);
        if (!fn && vt) fn = vt[kBuildBlock];
        if (!fn || !sdk_safe_ptr(fn, 16)) return false;
        using fn_t = void(__fastcall*)(void*, const IVec3*, unsigned char, bool, float);
        ((fn_t)fn)(gm, &pos, face, false, 1.0f);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline bool attack(void* gm, void** vt, Actor* target, const Vec3* clickPos) {
    if (!gm || !vt || !target || !clickPos) return false;
    __try {
        void* fn = vt[kAttack];
        if (!fn || !sdk_safe_ptr(fn, 16)) return false;
        using fn_t = void(__fastcall*)(void*, Actor*, const Vec3*);
        ((fn_t)fn)(gm, target, clickPos);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

enum class SwingSource : unsigned char {
    None      = 0,
    Build     = 1,
    Mine      = 2,
    Interact  = 3,
    Attack    = 4,
    UseItem   = 5,
    ThrowItem = 6,
    DropItem  = 7,
    Event     = 8,
};

inline bool swing(Actor* lp, SwingSource source = SwingSource::Attack, bool offhand = false) {
    if (!lp) return false;
    __try {
        void** vt = *(void***)lp;
        if (!vt || !sdk_safe_ptr(vt, ::Layout::VTable::Guard(::Layout::VTable::Actor_swing)))
            return false;
        void* fn = vt[::Layout::VTable::Actor_swing];
        if (!fn) return false;
        ((bool(__fastcall*)(Actor*, unsigned char, bool))fn)(lp, (unsigned char)source, offhand);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

struct CrosshairHit {
    HitType      type     = HitType::NOTHING;
    unsigned int entityId = 0;
    Vec3         pos{};              // exact world point the ray struck
    int          blockPos[3] = {0,0,0};
    unsigned char facing  = 0;
    bool         valid    = false;
};

inline bool readCrosshair(Level* level, CrosshairHit* out) {
    if (!level || !out) return false;
    __try {
        HitResult* hr = level->getHitResult();
        if (!hr || !sdk_safe_ptr(hr, ::Layout::Probe::HitResult)) return false;
        out->type        = hr->mType;
        out->entityId    = hr->mEntityId;
        out->pos         = hr->mPos;
        out->blockPos[0] = hr->mBlockPos[0];
        out->blockPos[1] = hr->mBlockPos[1];
        out->blockPos[2] = hr->mBlockPos[2];
        out->facing      = hr->mFacing;
        out->valid       = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline void prepEntityHit(Level* level) {
    if (!level) return;
    __try {
        HitResult* hr = level->getHitResult();
        if (hr && sdk_safe_ptr(hr, 0x20)) hr->mType = HitType::ENTITY;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

inline unsigned int actorEntityId(Actor* a) {
    if (!a) return 0xFFFFFFFFu;
    __try { return (unsigned int)a->getEntityId(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFFu; }
}

template <typename SkipFn>
inline Actor* actorFromEntityId(const std::vector<Actor*>& actors, Actor* lp,
                                unsigned int entityId, SkipFn&& skip) {
    if (entityId == 0xFFFFFFFFu) return nullptr;
    for (Actor* a : actors) {
        if (!a || a == lp) continue;
        if (!sdk_safe_ptr(a, ::Layout::Probe::Actor)) continue;
        if (skip(a)) continue;
        if (actorEntityId(a) == entityId) return a;
    }
    return nullptr;
}

// Prime Level HitResult as a BLOCK hit so the server accepts the transaction.
inline void prepBlockHit(Level* level, const IVec3& pos, int face,
                         float hitX = -1.f, float hitY = -1.f, float hitZ = -1.f) {
    if (!level) return;
    __try {
        HitResult* hr = level->getHitResult();
        if (!hr || !sdk_safe_ptr(hr, 0x40)) return;
        hr->mType = HitType::BLOCK;
        hr->mFacing = (unsigned char)face;
        hr->mBlockPos[0] = pos.x;
        hr->mBlockPos[1] = pos.y;
        hr->mBlockPos[2] = pos.z;
        if (hitX < 0.f) {
            hr->mPos = Vec3((float)pos.x + 0.5f, (float)pos.y + 0.5f, (float)pos.z + 0.5f);
        } else {
            hr->mPos = Vec3(hitX, hitY, hitZ);
        }
        hr->mRayDir = Vec3(0, -1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

} // namespace GameMode
} // namespace SDK
