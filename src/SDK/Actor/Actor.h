#pragma once
#include "../Types.h"
#include "MobEffect.h"
#include "ActorFlags.h"
#include "../../Memory/OffsetManager.h"
#include "../../Memory/SigManager.h"
#include <optional>
#include <vector>
#include <memory>
#include <map>
#include <mutex>
#include <unordered_map>
#include <entt/entt.hpp>

// ENTT integration
namespace SDK { enum class EntityId:std::uint32_t{}; }
struct EntityIdTraits{using value_type=SDK::EntityId;using entity_type=uint32_t;using version_type=uint16_t;static constexpr entity_type entity_mask=0x3FFFF,version_mask=0x3FFF;};
template<> struct entt::entt_traits<SDK::EntityId>:entt::basic_entt_traits<EntityIdTraits>{static constexpr std::size_t page_size=2048;};

namespace SDK {
    struct ActorTypeComponent { uint32_t type; };
    struct ActorOwnerComponent { class Actor* mActor; };
    struct EntityContext { void* registry; entt::basic_registry<EntityId>& enttRegistry; EntityId entity; };

    template<typename T> inline T* enttGetComp(void* sp,EntityId e){auto* st=(entt::basic_storage<T,EntityId>*)sp;__try{if(st->contains(e))return &st->get(e);}__except(EXCEPTION_EXECUTE_HANDLER){}return nullptr;}
}

#include "../../Memory/OffsetManager.h"

extern uintptr_t g_entt_assure;
// MoveInputComponent* getOrEmplace(EntityContext*) — the game's own accessor.
// Using it means movement code needs no component type hash and no pool math.
extern uintptr_t g_MoveInputComponent_get;
extern uintptr_t g_LocalPlayer_applyTurnDelta;
// Actor::setImmobile(EntityContext*, bool) — see Actor::setImmobile below.
extern uintptr_t g_Actor_setImmobile;

// POD-only so __try is legal (C2712). Calls the game's own flag setter.
inline bool sdk_setImmobileRaw(void* entityContext, bool value) {
    if (!g_Actor_setImmobile || !entityContext) return false;
    if (!sdk_safe_ptr(entityContext, 0x14)) return false;
    if (!sdk_safe_ptr((void*)g_Actor_setImmobile, 16)) return false;
    __try {
        using fn_t = void(__fastcall*)(void*, bool);
        ((fn_t)g_Actor_setImmobile)(entityContext, value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// entt "assure(registry, type_hash) -> storage*", ref (inside Actor::setImmobile):
//     mov rax,[rcx+8] / mov esi,[rcx+10h] / mov rcx,rax
//     mov edx, <type_hash> at +0x21 / call assure at +0x26
inline uintptr_t sdk_enttAssureFromSetImmobile() {
    if (!g_Actor_setImmobile) return 0;
    const uintptr_t site = g_Actor_setImmobile + 0x26;
    if (!sdk_safe_ptr((void*)site, 5)) return 0;
    unsigned char op = 0;
    int rel = 0;
    __try {
        op = *(unsigned char*)site;
        rel = *(int*)(site + 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (op != 0xE8) return 0;
    const uintptr_t target = site + 5 + (uintptr_t)(intptr_t)rel;
    return sdk_safe_ptr((void*)target, 16) ? target : 0;
}

// Sparse-set containment test, byte-for-byte the one Actor::setImmobile performs
// before adding/removing. Returns 1 = has component, 0 = does not, -1 = unknown.
inline int sdk_hasComponentRaw(void* entityContext, uintptr_t assureFn, unsigned int typeHash) {
    if (!assureFn || !entityContext || !sdk_safe_ptr(entityContext, 0x14)) return -1;
    __try {
        void* reg = *(void**)((uintptr_t)entityContext + 8);
        const unsigned int ent = *(unsigned int*)((uintptr_t)entityContext + 0x10);
        if (!reg || !sdk_safe_ptr(reg, sizeof(void*))) return -1;
        using assure_t = uintptr_t(__fastcall*)(void*, unsigned int);
        const uintptr_t storage = ((assure_t)assureFn)(reg, typeHash);
        if (!storage || !sdk_safe_ptr((void*)storage, 0x18)) return -1;
        const uintptr_t begin = *(uintptr_t*)(storage + 0x08);
        const uintptr_t end   = *(uintptr_t*)(storage + 0x10);
        if (!begin || end < begin) return -1;
        const unsigned int page = (ent & 0x3FFFFu) >> 11;
        const size_t pages = (size_t)((end - begin) >> 3);
        if (page >= pages) return 0;
        const uintptr_t pg = *(uintptr_t*)(begin + 8ull * page);
        if (!pg || !sdk_safe_ptr((void*)pg, 4ull * 0x800)) return 0;
        const unsigned int v = *(unsigned int*)(pg + 4ull * (ent & 0x7FFu));
        return ((v ^ (ent & 0xFFFC0000u)) <= 0x3FFFEu) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// entt type_hashes (mov edx, imm32)
// temp assets, todo move to sigs
static constexpr unsigned int kActorIsImmobileFlagHash = 0xAB4E40CBu;  // (Actor::setImmobile)
static constexpr unsigned int kMobIsImmobileFlagHash   = 0xA1BC775Au;
static constexpr unsigned int kActorDataFlagCompHash   = 0xC67426F3u;  // sub_141ED7DF0 / sub_141ED7610

extern uintptr_t g_entt_assure_actorDataFlag;

// ActorDataFlagComponent payload lookup, ref: storage = assure(registryRoot, 0xC67426F3);
//     idx = sparse[(entity & 0x3FFFF) >> 11][entity & 0x7FF], present = (idx ^ (entity & 0xFFFC0000)) <= 0x3FFFE
//     comp = *(void**)(storage[+0x50] + ((idx & 0x3FF80) >> 4)) + 24 * (idx & 0x7F)
inline void* sdk_actorDataFlagsRaw(void* entityContext, uintptr_t assureFn) {
    if (!assureFn || !entityContext || !sdk_safe_ptr(entityContext, 0x14)) return nullptr;
    if (!sdk_safe_ptr((void*)assureFn, 16)) return nullptr;
    __try {
        void* reg = *(void**)((uintptr_t)entityContext + 8);
        const unsigned int ent = *(unsigned int*)((uintptr_t)entityContext + 0x10);
        if (!reg || !sdk_safe_ptr(reg, sizeof(void*))) return nullptr;
        using assure_t = uintptr_t(__fastcall*)(void*, unsigned int);
        const uintptr_t storage = ((assure_t)assureFn)(reg, kActorDataFlagCompHash);
        if (!storage || !sdk_safe_ptr((void*)storage, 0x58)) return nullptr;

        const uintptr_t sparseBegin = *(uintptr_t*)(storage + 0x08);
        const uintptr_t sparseEnd   = *(uintptr_t*)(storage + 0x10);
        if (!sparseBegin || sparseEnd < sparseBegin) return nullptr;
        const unsigned int page = (ent & 0x3FFFFu) >> 11;
        if ((size_t)page >= (size_t)((sparseEnd - sparseBegin) >> 3)) return nullptr;
        const uintptr_t pg = *(uintptr_t*)(sparseBegin + 8ull * page);
        if (!pg || !sdk_safe_ptr((void*)pg, 4ull * 0x800)) return nullptr;
        const unsigned int idx = *(unsigned int*)(pg + 4ull * (ent & 0x7FFu));
        if ((idx ^ (ent & 0xFFFC0000u)) > 0x3FFFEu) return nullptr;  // not in the set

        const uintptr_t payloadPages = *(uintptr_t*)(storage + 0x50);
        if (!payloadPages || !sdk_safe_ptr((void*)payloadPages, sizeof(void*))) return nullptr;
        const uintptr_t chunk = *(uintptr_t*)(payloadPages + ((idx & 0x3FF80u) >> 4));
        if (!chunk) return nullptr;
        void* comp = (void*)(chunk + 24ull * (idx & 0x7Fu));
        return sdk_safe_ptr(comp, 16) ? comp : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Read/write one bit of the flag bitset. `bit` is an ActorFlags index (0..0x71).
// Returns -1 when the component could not be read. Both are POD-only + SEH.
inline int sdk_actorFlagGetRaw(void* comp, int bit) {
    if (!comp || bit < 0 || bit >= 0x72) return -1;
    if (!sdk_safe_ptr(comp, 16)) return -1;
    __try {
        const unsigned long long w = *(unsigned long long*)((uintptr_t)comp + 8ull * (bit >> 6));
        return (int)((w >> (bit & 63)) & 1ull);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
inline bool sdk_actorFlagSetRaw(void* comp, int bit, bool value) {
    if (!comp || bit < 0 || bit >= 0x72) return false;
    if (!sdk_safe_ptr(comp, 16)) return false;
    __try {
        auto* w = (unsigned long long*)((uintptr_t)comp + 8ull * (bit >> 6));
        const unsigned long long mask = 1ull << (bit & 63);
        if (value) *w |= mask; else *w &= ~mask;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// SEH entt helpers (POD only). The registry may legitimately be absent or
// half-built (before world join, dimension change, stale pointer); a fault is
// a deliberate probe -> "no component", never report it as a crash.
inline void* entt_assure_seh(uintptr_t fn,void* reg,uint32_t hash){
    if(!fn||!reg)return nullptr;
    CrashHandler::EnterProbe();
    __try{void* r=((void*(__fastcall*)(void*,uint32_t))fn)(reg,hash);
          CrashHandler::LeaveProbe(); return r;}
    __except(EXCEPTION_EXECUTE_HANDLER){CrashHandler::LeaveProbe(); return nullptr;}
}
inline int entt_get_actors_seh(void* sp,void** out,int max){
    auto* st=(entt::basic_storage<SDK::ActorOwnerComponent,SDK::EntityId>*)sp;int n=0;
    CrashHandler::EnterProbe();
    __try{size_t sz=st->size();const SDK::EntityId* e=st->data();if(e&&sz>0)for(size_t i=0;i<sz&&n<max;++i)if(st->contains(e[i])){auto&c=st->get(e[i]);if(c.mActor)out[n++]=(void*)c.mActor;}}__except(EXCEPTION_EXECUTE_HANDLER){}
    CrashHandler::LeaveProbe();
    return n;
}

// SEH-guarded, POD-only walk of the MobEffectsComponent vector; reads at most
// maxN effects into parallel arrays, returns the count. Bails to 0 on layout
// mismatch (span not a multiple of the 0x88 stride). Hash 0xE6A1B550 on 1.26.30.05.
inline int mobeffects_read_seh(void* comp, uint32_t* ids, int* amps, int* durs, unsigned char* infinite, int maxN){
    if(!comp || !sdk_safe_ptr(comp, 0x18)) return 0;
    const int offDur = Offsets().EffectInstance_duration;
    const int offAmp = Offsets().EffectInstance_amplifier;
    const int offNc  = Offsets().EffectInstance_noCounter;
    const int elem   = Offsets().EffectInstance_size;
    if (offDur <= 0 || offAmp <= 0 || offNc <= 0 || elem < 0x40) return 0;
    int n = 0;
    __try {
        uintptr_t begin = *(uintptr_t*)comp;
        uintptr_t end   = *(uintptr_t*)((uintptr_t)comp + 0x8);
        if(!begin || end < begin) return 0;
        uintptr_t span = end - begin;
        if(span == 0 || (span % (uintptr_t)elem) != 0) return 0;
        int cnt = (int)(span / (uintptr_t)elem);
        if(cnt < 0 || cnt > 64) return 0;
        for(int i = 0; i < cnt && n < maxN; ++i){
            uintptr_t e = begin + (uintptr_t)i * (uintptr_t)elem;
            if(!sdk_safe_ptr((void*)e, (size_t)offNc + 1)) break;
            uint32_t id  = *(uint32_t*)e;
            int dur      = *(int*)(e + (uintptr_t)offDur);
            int amp      = *(int*)(e + (uintptr_t)offAmp);
            unsigned char nc = *(unsigned char*)(e + (uintptr_t)offNc);
            if(id == 0 || id > 36) continue;
            if(amp < 0 || amp > 255) amp = 0;
            ids[n] = id; amps[n] = amp; durs[n] = dur;
            infinite[n] = (unsigned char)((nc != 0 || dur <= 0) ? 1 : 0);
            ++n;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { return n; }
    return n;
}

inline void actor_vtable_getname_seh(void* fn, void* actor, std::string& out){
    __try { const std::string& r = ((const std::string&(__fastcall*)(void*))fn)(actor); out = r; }
    __except(EXCEPTION_EXECUTE_HANDLER) {}
}

namespace SDK {

class SerializedSkinBase; class SerializedSkin; class PlayerInventory; class Container; class ItemStack;

// Attributes (health / movement speed) — 1.26.50.04: the recompile changed the
// component layout AND the per-attribute ids, so the old Latite 1.26.30 vector
// model is dead. AttributesComponent (entt hash 0xFD3B0613) is an
// AttributeInstanceHandle — a SORTED FLAT MAP id -> AttributeInstance; the
// element stride is 0x50, and an entry is binary-searched in idsBegin..idsEnd
// then read at valsBegin[0x88 * index] (layout below from getInstance,
// sub_1431A00A0). AttributeInstance is 0x88 bytes; max value at +0x70, CURRENT
// at +0x7C (cf. getMovementSpeed sub_1401E38C0: `movss xmm0, [rax+7Ch]`).
struct AttributesComponentRaw {
    static constexpr uint32_t type_hash = 0xFD3B0613u;
    uint32_t* idsBegin;   // +0x00
    uint32_t* idsEnd;     // +0x08
    uint32_t* idsCap;     // +0x10
    char*     valsBegin;  // +0x18, element stride 0x88
    char*     valsEnd;    // +0x20
    char*     valsCap;    // +0x28
};
static_assert(sizeof(AttributesComponentRaw) == 0x30);

// SEH-guarded element lookup. `key` is SigManager::AttributeKey_Health/_Movement;
// its +0x4 uint32 is the runtime attribute id (read from the game, so recompile-
// safe). Returns the 0x88-byte AttributeInstance or nullptr. POD-only (C2712).
inline void* sdk_attributesEntryRaw(void* entityContext, uintptr_t assureFn, uintptr_t key) {
    if (!assureFn || !entityContext || !key) return nullptr;
    if (!sdk_safe_ptr(entityContext, 0x14)) return nullptr;
    if (!sdk_safe_ptr((void*)assureFn, 16)) return nullptr;
    if (!sdk_safe_ptr((void*)key, 8)) return nullptr;
    __try {
        void* reg = *(void**)((uintptr_t)entityContext + 8);
        const unsigned int ent = *(unsigned int*)((uintptr_t)entityContext + 0x10);
        if (!reg || !sdk_safe_ptr(reg, sizeof(void*))) return nullptr;
        using assure_t = uintptr_t(__fastcall*)(void*, unsigned int);
        const uintptr_t storage = ((assure_t)assureFn)(reg, AttributesComponentRaw::type_hash);
        if (!storage || !sdk_safe_ptr((void*)storage, 0x58)) return nullptr;

        // sparse-set containment (identical to sdk_actorDataFlagsRaw; entt
        // basic_storage layout is type-independent).
        const uintptr_t sparseBegin = *(uintptr_t*)(storage + 0x08);
        const uintptr_t sparseEnd   = *(uintptr_t*)(storage + 0x10);
        if (!sparseBegin || sparseEnd < sparseBegin) return nullptr;
        const unsigned int page = (ent & 0x3FFFFu) >> 11;
        if ((size_t)page >= (size_t)((sparseEnd - sparseBegin) >> 3)) return nullptr;
        const uintptr_t pg = *(uintptr_t*)(sparseBegin + 8ull * page);
        if (!pg || !sdk_safe_ptr((void*)pg, 4ull * 0x800)) return nullptr;
        const unsigned int idx = *(unsigned int*)(pg + 4ull * (ent & 0x7FFu));
        if ((idx ^ (ent & 0xFFFC0000u)) > 0x3FFFEu) return nullptr;  // not in the set

        const uintptr_t payloadPages = *(uintptr_t*)(storage + 0x50);
        if (!payloadPages || !sdk_safe_ptr((void*)payloadPages, sizeof(void*))) return nullptr;
        const uintptr_t chunk = *(uintptr_t*)(payloadPages + ((idx & 0x3FF80u) >> 4));
        if (!chunk) return nullptr;
        AttributesComponentRaw* comp =
            (AttributesComponentRaw*)(chunk + 0x50ull * (idx & 0x7Fu));
        if (!sdk_safe_ptr(comp, sizeof(AttributesComponentRaw))) return nullptr;

        uint32_t* b = comp->idsBegin;
        uint32_t* e = comp->idsEnd;
        if (!b || e < b) return nullptr;
        if (!sdk_safe_ptr(b, 4) || !sdk_safe_ptr(e, 4)) return nullptr;
        const size_t n = (size_t)(e - b);
        if (n == 0 || n > 64) return nullptr;          // sanity: a mob has < 64 attrs
        if (!comp->valsBegin) return nullptr;
        if (!sdk_safe_ptr(comp->valsBegin, n * 0x88)) return nullptr;

        const uint32_t id = *(uint32_t*)((uintptr_t)key + 4);
        // std::lower_bound, the same comparison the game's getInstance uses.
        uint32_t* lo = b;
        uint32_t* hi = e;
        while (lo < hi) {
            uint32_t* mid = lo + ((size_t)(hi - lo) >> 1);
            if (*mid < id) lo = mid + 1;
            else hi = mid;
        }
        if (lo == e || *lo != id) return nullptr;      // attribute not present
        return (void*)(comp->valsBegin + (size_t)(lo - b) * 0x88);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

struct IEntityComponentBase { }; // entt component tag base (empty)
struct AttributesComponent {
    static constexpr uint32_t type_hash = 0xFD3B0613u;
};

// ActorEquipmentComponent — Latite 1.26.30 layout (IEntityComponent is an empty
// tag base): hand @ +0x0 / armor @ +0x8, struct exactly 16 bytes — that size
// MUST match the game's stride or entt get() indexes the wrong element.
// armor = 4 worn pieces (0=helmet, 1=chestplate, 2=leggings, 3=boots).
struct ActorEquipmentComponent {
    static constexpr uint32_t type_hash = 0xB06141A9u;
    void* handContainer;   // Container* (offhand/hand slots)
    void* armorContainer;  // Container* (worn armor)
};

// MobEffectData is defined in MobEffect.h (included via SDK.h / forward use).

// ActorDataFlagComponent — std::bitset<0x77> + bookkeeping. The "is immobile"
// predicate (sub_141ED7610) reads bit 16 = Noai of flags[0] (`*flags & 0x10000`).
// Game storage stride is 24 bytes (indexed as `24 * (idx & 0x7F)`), so this
// struct MUST be exactly 0x18 bytes or entt get() indexes the wrong element.
struct ActorDataFlagComponentRaw {
    static constexpr uint32_t type_hash = 0xC67426F3u;
    uint64_t flags[3]; // 0x18 bytes total
};

// MobEffectsComponent = std::vector<MobEffectInstance> at +0x0, modeled as three
// raw pointers (0x18 bytes) to match the game's entt stride and avoid running a
// std::vector dtor on foreign memory. Hash 0xE6A1B550 @ 1.26.30.05, stride 0x88.
struct MobEffectsComponentRaw {
    static constexpr uint32_t type_hash = 0xE6A1B550u;
    void* begin; void* end; void* cap;
};

class Actor; // fwd

class Actor {
public:
    // Direct component-pointer fields (Latite CLASS_FIELD offsets for 1.26.30,
    // dereferenced by every working client). Do NOT switch these to entt tryGet:
    // storage.get returned garbage on 1.26.30.05 and CRASHED HitBoxes on enable.
    StateVectorComponent* getStateVector(){
        int off = Offsets().Actor_mStateVector ? Offsets().Actor_mStateVector : 0x240;
        auto* p = (StateVectorComponent**)((uintptr_t)this + off);
        if (!sdk_safe_ptr(p, sizeof(void*))) return nullptr;
        StateVectorComponent* c = *p;
        return (c && sdk_safe_ptr(c, sizeof(StateVectorComponent))) ? c : nullptr;
    }
    AABBShapeComponent* getAabbs(){
        int off = Offsets().Actor_mAABB ? Offsets().Actor_mAABB : 0x248;
        auto* p = (AABBShapeComponent**)((uintptr_t)this + off);
        if (!sdk_safe_ptr(p, sizeof(void*))) return nullptr;
        AABBShapeComponent* c = *p;
        return (c && sdk_safe_ptr(c, sizeof(AABBShapeComponent))) ? c : nullptr;
    }
    ActorRotationComponent* getRotation(){
        int off = Offsets().Actor_mRotation ? Offsets().Actor_mRotation : 0x250;
        auto* p = (ActorRotationComponent**)((uintptr_t)this + off);
        if (!sdk_safe_ptr(p, sizeof(void*))) return nullptr;
        ActorRotationComponent* c = *p;
        return (c && sdk_safe_ptr(c, sizeof(ActorRotationComponent))) ? c : nullptr;
    }
    ActorHeadRotationComponent* getHeadRotation(){
        auto* c = tryGet<ActorHeadRotationComponent>(0xBABE7211u);
        return (c && sdk_safe_ptr(c, sizeof(ActorHeadRotationComponent))) ? c : nullptr;
    }
    MobBodyRotationComponent* getBodyRotation(){
        auto* c = tryGet<MobBodyRotationComponent>(0x9442AD10u);
        return (c && sdk_safe_ptr(c, sizeof(MobBodyRotationComponent))) ? c : nullptr;
    }
    void applyTurnDelta(Vec2 delta) {
        if (!g_LocalPlayer_applyTurnDelta) return;
        __try {
            using fn_t = void(__fastcall*)(Actor*, Vec2*);
            ((fn_t)g_LocalPlayer_applyTurnDelta)(this, &delta);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    void applyTurnDeltaToTarget(float targetPitch, float targetYaw) {
        ActorRotationComponent* rc = getRotation();
        if (!rc) return;
        float dPitch = rc->rotation.x - targetPitch;
        float dYaw = targetYaw - rc->rotation.y;
        while (dYaw >  180.0f) dYaw -= 360.0f;
        while (dYaw < -180.0f) dYaw += 360.0f;
        applyTurnDelta({ dPitch, dYaw });
    }
    AABB getBoundingBox(){auto*c=getAabbs();return c?AABB{c->mMin,c->mMax}:AABB{};}
    Vec2 getRot(){auto*c=getRotation();return c?c->rotation:Vec2{};}
    Vec3 getPos(){auto*c=getStateVector();return c?c->pos:Vec3{};}
    // Current health (e.g. 0..20), nullopt if unavailable. Reads the game's own
    // Health AttributeInstanceHandle key; value = the +0x7C current field of the
    // 0x88-byte entry.
    std::optional<float> getHealth() {
        void* entry = sdk_attributesEntryRaw(
            (void*)((uintptr_t)this + 0x8), g_entt_assure,
            SigManager::GetInstance().AttributeKey_Health);
        if (!entry || !sdk_safe_ptr(entry, 0x88)) return std::nullopt;
        float v = 0.0f;
        __try { v = *(float*)((uintptr_t)entry + 0x7C); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return std::nullopt; }
        if (v < 0.0f || v > 1024.0f) return std::nullopt; // sanity
        return v;
    }
    // Current RAW movement-speed attribute (vanilla base ~0.10; Speed effects
    // raise, Slowness lowers). Same lookup as getHealth with the game's Movement
    // key; id and offset are read from the binary — no enum drift.
    std::optional<float> getMovementSpeed() {
        void* entry = sdk_attributesEntryRaw(
            (void*)((uintptr_t)this + 0x8), g_entt_assure,
            SigManager::GetInstance().AttributeKey_Movement);
        if (!entry || !sdk_safe_ptr(entry, 0x88)) return std::nullopt;
        float v = 0.0f;
        __try { v = *(float*)((uintptr_t)entry + 0x7C); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return std::nullopt; }
        if (v <= 0.0f || v > 5.0f) return std::nullopt; // sanity: vanilla base ~0.1
        return v;
    }

    Vec3 getPosOld(){auto*c=getStateVector();return c?c->posOld:Vec3{};}
    // Client-side game type — ActorGameTypeComponent (entt hash 0x88D3EDDF,
    // FNV-1a; verified @ 1.26.42.01). int values: Survival=0, Creative=1,
    // Adventure=2, SurvivalSpec=3, CreativeSpec=4, Default=5, Spectator=6.
    // Spectator (6) = the engine's own local noclip (fly + skip block
    // collision); purely client-side, the server's game type is unchanged.
    int getGameType(){
        auto* c = tryGet<int>(0x88D3EDDFu);
        if (!c || !sdk_safe_ptr(c, sizeof(int))) return -1;
        int t; __try { t = *c; } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
        return (t >= 0 && t <= 6) ? t : -1;
    }
    bool setGameType(int t){
        if (t < 0 || t > 6) return false;
        auto* c = tryGet<int>(0x88D3EDDFu);
        if (!c || !sdk_safe_ptr(c, sizeof(int))) return false;
        __try { *c = t; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // Offhand container — ActorEquipmentComponent::handContainer (hash
    // 0xB06141A9; hand @ +0x0 / armor @ +0x8). The OFFHAND stack is slot 1 of
    // the hand container (slot 0 = main hand). nullptr = unreadable = "unknown".
    void* getOffhandContainer(){
        auto* c = tryGet<ActorEquipmentComponent>(ActorEquipmentComponent::type_hash);
        if (!c || !sdk_safe_ptr(c, sizeof(ActorEquipmentComponent))) return nullptr;
        if (!c->handContainer || !sdk_safe_ptr(c->handContainer, sizeof(void*))) return nullptr;
        return c->handContainer;
    }
    // 1.26.50.04: the Actor::setSprinting vtable slot is unpinned (old slot 0x8B
    // is an unrelated float getter in LocalPlayer vtable 0x14E8E0BC0), so route
    // through setStatusFlag — bit 3 is what the game's own flag getters read.
    void setSprinting(bool v){ setStatusFlag(ActorFlags::Sprinting, v); }
    bool isPlayer(){
        // Primary: entt ActorTypeComponent (319 = Player, 63 = local). On
        // 1.26.30.05 the storage walk can return non-null garbage, so trust it
        // only on a positive "player" — else fall through to the name probe.
        auto*c=tryGet<ActorTypeComponent>(0x4F6BA419);
        if(c && (c->type==319||c->type==63)) return true;
        // Fallback: players always carry a printable name std::string at +0xBC0;
        // smaller non-players fail the SEH read, so this stays a player-only signal.
        std::string n;
        auto*np=(std::string*)((uintptr_t)this+::Layout::Actor::LocalName);
        if(sdk_safe_string(np,n) && n.size()>=1 && n.size()<=64){
            bool ok=true;
            for(unsigned char ch:n){ if(ch<0x20||ch==0x7F){ ok=false; break; } }
            if(ok) return true;
        }
        return false;
    }
    // RUNTIME id. The entt tryGet<uint64_t> walk reads garbage on this build
    // family (drifted page layout) and silently returns 0, breaking id matches
    // downstream. Uses the game's own accessor instead, g_Actor_getUniqueID:
    //     void getUniqueID(Actor* this, struct { u64 id; ... } * out)
    // SEH-guarded; falls back to the entt walk when the sig is unresolved.
    uint64_t getRuntimeID(){
        extern uintptr_t g_Actor_getUniqueID;
        if (g_Actor_getUniqueID && sdk_safe_ptr((void*)g_Actor_getUniqueID, 16)) {
            unsigned char out[16] = {};
            __try {
                ((void(__fastcall*)(void*, void*))g_Actor_getUniqueID)(this, out);
                uint64_t id;
                memcpy(&id, out, sizeof(id));
                if (id != 0) return id;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        auto*c=tryGet<uint64_t>(0xFC0DBBB5);return c?*c:0;
    }
    // Bedrock actor type id via ActorTypeComponent (hash 0x4F6BA419): 319 =
    // Player, 63 = local, 64 = dropped item, else some mob/projectile. Unlike
    // getUsername() it never touches the player-list map, so it is safe on ANY
    // thread including the render thread. Returns -1 when absent/unreadable.
    int getActorTypeId() {
        __try {
            auto* c = tryGet<ActorTypeComponent>(0x4F6BA419);
            if (!c || !sdk_safe_ptr(c, sizeof(ActorTypeComponent))) return -1;
            const uint32_t t = c->type;
            if (t > 1000000u) return -1;
            return static_cast<int>(t);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }
    // Actor::ticksExisted (@0x198, Latite 1.26.30), +1 per GAME tick (20 Hz).
    // Gate module logic with it: OnUpdate runs on the render update, so wall-
    // clock timing can fire twice inside one PlayerAuthInput flush (batch kicks).
    long long getTicksExisted(){
        auto* p=(int32_t*)((uintptr_t)this+::Layout::Actor::TicksExisted);
        if(!sdk_safe_ptr(p,sizeof(int32_t)))return -1;
        int32_t t=*p;
        return (t>=0)?(long long)t:-1;
    }
    // Player::supplies (PlayerInventory*) @ 0x5B8 (Latite 1.26.30). Valid on the
    // local player. Defined inline in SDK.h (needs the complete type).
    PlayerInventory* getSupplies();
    // Open container model while chest/UI is open. Defined in SDK.h.
    class ContainerManagerModel* getContainerManagerModel();
    // Worn armor for slot 0=helmet,1=chestplate,2=leggings,3=boots, from
    // ActorEquipmentComponent (hash 0xB06141A9) -> armorContainer. Defined
    // inline in SDK.h (needs complete Container/ItemStack). nullptr = unavailable.
    ItemStack* getArmorItem(int slot);

    // Active effects from the entt MobEffectsComponent (hash 0xE6A1B550 @
    // 1.26.30.05); empty list when the component is absent or drifted. Safe to
    // call every frame.
    std::vector<MobEffectData> getStatusEffects(){
        std::vector<MobEffectData> out;
        auto* comp = tryGet<MobEffectsComponentRaw>(MobEffectsComponentRaw::type_hash);
        if(!comp) return out;
        uint32_t ids[64]; int amps[64]; int durs[64]; unsigned char inf[64];
        int n = mobeffects_read_seh(comp, ids, amps, durs, inf, 64);
        out.reserve(n);
        for(int i = 0; i < n; ++i)
            out.push_back(MobEffectData{ ids[i], amps[i], durs[i], inf[i] != 0 });
        return out;
    }
    // Raw MobEffectsComponent pointer (std::vector<MobEffectInstance> at +0x0,
    // element stride Layout::EffectInstance_size) for writers — AntiEffect
    // neutralizes blocked effects in place. nullptr when absent/unreadable.
    void* getMobEffectsComponentRaw(){
        auto* c = tryGet<MobEffectsComponentRaw>(MobEffectsComponentRaw::type_hash);
        return (c && sdk_safe_ptr(c, 0x18)) ? c : nullptr;
    }
    EntityId getEntityId(){auto*ctx=(EntityContext*)((uintptr_t)this+0x8);return(ctx&&sdk_safe_ptr(ctx,sizeof(EntityContext)))?ctx->entity:EntityId{0};}
    // OnGround flag component (entt hash 0xC29078A0): presence == standing on
    // ground. Same sparse-set test AirJump's emplace path round-trips through.
    bool isOnGround(){
        if(!g_entt_assure)return false;
        return sdk_hasComponentRaw((void*)((uintptr_t)this+0x8),g_entt_assure,0xC29078A0u)==1;
    }

    // Immobile flag — an entt tag component (ActorIsImmobileFlagComponent,
    // type_hash 0xAB4E40CB), not an ActorData bitflag. Calls the game's own
    // setter (Actor::setImmobile via SigManager) so registry bookkeeping stays
    // consistent; false removes the component. Client-side only, no packet.
    void setImmobile(bool value){
        // The setter takes the Actor's EntityContext, not the Actor: reversed as
        // `mov rax,[rcx+8]` (registry) / `mov esi,[rcx+10h]` (entity), which is
        // exactly this SDK's EntityContext layout at Actor+0x8.
        sdk_setImmobileRaw((void*)((uintptr_t)this + 0x8), value);
    }
    void clearImmobile(){ setImmobile(false); }
    // 1 = flag present, 0 = absent, -1 = could not tell. `assureFn` comes from
    // sdk_enttAssureFromSetImmobile() (cache it; it never changes at runtime).
    int hasImmobile(uintptr_t assureFn){
        return sdk_hasComponentRaw((void*)((uintptr_t)this + 0x8), assureFn, kActorIsImmobileFlagHash);
    }
    int hasMobImmobile(uintptr_t assureFn){
        return sdk_hasComponentRaw((void*)((uintptr_t)this + 0x8), assureFn, kMobIsImmobileFlagHash);
    }

    // Noai (bit 16) makes movement skip the whole travel-vector update and a
    // second system zero the velocity — the frozen state servers apply; that is
    // the flag to clear. 1 = set, 0 = clear, -1 = unreadable. Reads the
    // ActorDataFlagComponent (hash 0xC67426F3) through the same entt path as
    // getHealth; the g_entt_assure_actorDataFlag accessor is called with a
    // different container argument by the game and returns -1 here. Raw-walk
    // fallback when tryGet misses.
    int getStatusFlag(ActorFlags flag){
        const int bit = (int)flag;
        if (bit < 0 || bit >= 0x72) return -1;
        if (auto* c = tryGet<ActorDataFlagComponentRaw>(ActorDataFlagComponentRaw::type_hash)) {
            if (sdk_safe_ptr(c, sizeof(ActorDataFlagComponentRaw)))
                return (int)((c->flags[bit >> 6] >> (bit & 63)) & 1ull);
        }
        void* c = sdk_actorDataFlagsRaw((void*)((uintptr_t)this + 0x8), g_entt_assure_actorDataFlag);
        return c ? sdk_actorFlagGetRaw(c, bit) : -1;
    }
    bool setStatusFlag(ActorFlags flag, bool value){
        const int bit = (int)flag;
        if (bit < 0 || bit >= 0x72) return false;
        if (auto* c = tryGet<ActorDataFlagComponentRaw>(ActorDataFlagComponentRaw::type_hash)) {
            if (sdk_safe_ptr(c, sizeof(ActorDataFlagComponentRaw))) {
                const uint64_t mask = 1ull << (bit & 63);
                if (value) c->flags[bit >> 6] |= mask; else c->flags[bit >> 6] &= ~mask;
                return true;
            }
        }
        void* c = sdk_actorDataFlagsRaw((void*)((uintptr_t)this + 0x8), g_entt_assure_actorDataFlag);
        return c && sdk_actorFlagSetRaw(c, bit, value);
    }
    SerializedSkin* getSkin();

    // Display / nametag / local name field (may include ranks & colors).
    std::string getDisplayName(){
        std::string n;
        if(isPlayer()){auto*np=(std::string*)((uintptr_t)this+::Layout::Actor::LocalName);if(sdk_safe_string(np,n)&&!n.empty())return n;}
        // The legacy name-tag virtual now has a boolean return ABI.
        return n.empty()?"Player":n;
    }
    std::string getUsername();
    std::string getRawName(){ return getUsername(); }
    uint64_t getUniqueID(){
        auto*c=tryGet<uint64_t>(0xA51E481Au);
        return c?*c:0;
    }

    std::vector<Actor*> getActorListFromRegistry(){
        std::vector<Actor*> out; if(!g_entt_assure)return out;
        void* reg=*(void**)((uintptr_t)this+0x10); if(!reg||!sdk_safe_ptr(reg,sizeof(void*)))return out;
        void* sp=entt_assure_seh(g_entt_assure,reg,0x132A5818u); if(!sp||!sdk_safe_ptr(sp,sizeof(void*)*4))return out;
        void* buf[4096]; int n=entt_get_actors_seh(sp,buf,4096);
        out.reserve(n); for(int i=0;i<n;++i)if(buf[i]&&sdk_safe_ptr(buf[i],sizeof(void*)))out.push_back((Actor*)buf[i]);
        return out;
    }

    // RenderPositionComponent — per-frame interpolated position the model pass
    // renders the actor AT; the LOCAL player is rendered from this component,
    // NOT from the pos/posOld lerp. Bare vec3 payload. Hash 0xE53C7221
    // (FNV-1a, = 3845943841), verified @ 1.26.42.01 vs serialization sub_144B6F660.
    struct RenderPositionComponent { Vec3 mPosition; };
    // SEH-free accessor (entt helpers carry their own __try); returns false
    // when the component is absent or unreadable.
    bool getRenderPosition(Vec3* out) {
        if (!out) return false;
        auto* c = tryGet<RenderPositionComponent>(0xE53C7221u);
        if (!c || !sdk_safe_ptr(c, sizeof(Vec3))) return false;
        __try { *out = c->mPosition; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
private:
    template<typename T> T* tryGet(uint32_t hash){
        if(!g_entt_assure)return nullptr;
        auto*ctx=(EntityContext*)((uintptr_t)this+0x8);if(!ctx||!sdk_safe_ptr(ctx,sizeof(EntityContext)))return nullptr;
        void* reg=*(void**)((uintptr_t)ctx+8);if(!reg)return nullptr;
        // tryGet runs concurrently on the game thread and the render thread;
        // an unguarded unordered_map under a concurrent read+insert is heap
        // corruption / ACCESS_VIOLATION. This mutex is what prevents that.
        static std::unordered_map<void*,std::unordered_map<uint32_t,void*>> cache;
        static std::mutex cacheMutex;
        void* sp=nullptr;
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            auto rit=cache.find(reg);if(rit!=cache.end()){auto hit=rit->second.find(hash);if(hit!=rit->second.end())sp=hit->second;}
            if(!sp){sp=entt_assure_seh(g_entt_assure,reg,hash);cache[reg][hash]=sp;}
        }
        return(sp&&sdk_safe_ptr(sp,sizeof(void*)))?enttGetComp<T>(sp,ctx->entity):nullptr;
    }
};

} // SDK
