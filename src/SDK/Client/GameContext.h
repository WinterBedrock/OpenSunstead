#pragma once
// Game context resolution + SEH-guarded actor access.
// resolveContext walks ClientInstance -> Minecraft -> Level -> LocalPlayer with
// sdk_safe_ptr probes; sehGetActorList / sehIsPlayer / sehGetPos /
// sehGetActorBox / forEachPlayer wrap the common actor reads. Needs the
// complete Actor + ClientInstance/Level types, so SDK.h includes it after
// those headers.

#include "../Types.h"
#include "ClientInstance.h"
#include "../Actor/Actor.h"
#include "../../Memory/OffsetManager.h"
#include "../../Core/Logger.h"
#include <cstdio>
#include <vector>

namespace SDK {

// Resolved per-frame game context. Any pointer here has already passed its
// sdk_safe_ptr probe, so callers may use them directly.
struct GameContext {
    ClientInstance* ci    = nullptr;
    Level*          level = nullptr;
    Actor*          lp    = nullptr; // local player
};

// Walk ClientInstance -> Minecraft -> Level -> LocalPlayer. Returns false (and
// leaves `out` untouched) if any hop is missing/unreadable; `needLevel` can be
// cleared for callers that only need ci + local player. The whole walk sits
// in a POD-only SEH frame (GameContext is three raw pointers — MSVC C2712), so
// a stale pointer degrades to a no-op. All offsets are pinned for 1.26.50.04,
// so a failure is runtime state (loading screen, no level yet), not drift.
// resolveContextDiag logs the failed hop, throttled to one line per cause.
inline void resolveContextDiag(int hop) {
#ifdef SUNSTEAD_LOGGING
    static int s_lastHop = -1;
    static int s_sinceLast = 0;
    if (hop == s_lastHop) {
        if (++s_sinceLast < 300) return;   // same cause: one line every ~300 calls
        s_sinceLast = 0;                   // reset, or the counter saturates and it logs every call
    } else {
        s_sinceLast = 0;
    }
    s_lastHop = hop;
    const char* hops[] = {
        "ClientInstance::get()",           // 0
        "ClientInstance::getMinecraft()",   // 1
        "Minecraft::getLevel()",            // 2
        "ClientInstance::getLocalPlayer()", // 3
        "SEH exception during walk",        // 4
    };
    if (hop >= 0 && hop < 5)
        Logger::Warn(std::string("resolveContext failed at ") + hops[hop], "GameContext");
#else
    (void)hop;
#endif
}

inline bool resolveContext(GameContext& out, bool needLevel = true) {
    __try {
        ClientInstance* ci = ClientInstance::get();
        if (!ci || !sdk_safe_ptr(ci, Layout::Probe::ClientInstance)) { resolveContextDiag(0); return false; }
        Minecraft* mc = ci->getMinecraft();
        if (!mc || !sdk_safe_ptr(mc, Layout::Probe::Minecraft)) { resolveContextDiag(1); return false; }
        Level* level = nullptr;
        if (needLevel) {
            level = mc->getLevel();
            // Empty on the main menu and during world load: no level yet, not
            // offset drift — stays quiet.
            if (!level || !sdk_safe_ptr(level, Layout::Probe::Level)) return false;
        }
        Actor* lp = ci->getLocalPlayer();
        if (!lp || !sdk_safe_ptr(lp, Layout::Probe::Actor)) { resolveContextDiag(3); return false; }
        out.ci = ci; out.level = level; out.lp = lp;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { resolveContextDiag(4); return false; }
}

// Out-parameter form for callers whose frame must stay POD (an SEH __try frame
// cannot hold objects with destructors, C2712).
inline bool resolveContext(ClientInstance** ciOut, Level** levelOut, Actor** lpOut) {
    GameContext ctx;
    if (!resolveContext(ctx, levelOut != nullptr)) return false;
    if (ciOut)    *ciOut = ctx.ci;
    if (levelOut) *levelOut = ctx.level;
    if (lpOut)    *lpOut = ctx.lp;
    return true;
}

// SEH-guarded fetch of the runtime actor list into a caller-owned vector.
inline bool sehGetActorList(Level* level, std::vector<Actor*>* out) {
    if (!level || !out) return false;
    __try { level->getRuntimeActorListInto(out); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// SEH-guarded Actor::isPlayer with the standard readability probe.
inline bool sehIsPlayer(Actor* actor) {
    if (!actor || !sdk_safe_ptr(actor, Layout::Probe::Actor)) return false;
    __try { return actor->isPlayer(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// SEH-guarded Actor::getPos.
inline bool sehGetPos(Actor* a, Vec3* out) {
    if (!a || !out) return false;
    __try { *out = a->getPos(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// SEH-guarded Actor::getPos + Actor::getBoundingBox in one read (overlay
// modules need both: the box supplies the height, the pos the anchor).
inline bool sehGetPosBox(Actor* a, Vec3* posOut, AABB* bbOut) {
    if (!a || !posOut || !bbOut) return false;
    __try { *posOut = a->getPos(); *bbOut = a->getBoundingBox(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Plausible actor dimensions, or the vanilla player box as a fallback. A
// recycled/partially-initialised actor can report a degenerate AABB, which
// produces overlays stretched across the screen.
inline Vec3 clampActorDim(const AABB& bb) {
    Vec3 dim = bb.higher - bb.lower;
    if (!(dim.x > 0.05f && dim.x < 6.0f) || !(dim.y > 0.1f && dim.y < 6.0f) ||
        !(dim.z > 0.05f && dim.z < 6.0f)) return Vec3(0.6f, 1.8f, 0.6f);
    return dim;
}

// Build a sane world-space hitbox for an actor: pull the AABB dimensions,
// clamp them to plausible player bounds (fallback 0.6 x 1.8 x 0.6), recompute
// the feet Y, and emit the min/max corners. Returns false on a bad read.
inline bool sehGetActorBox(Actor* a, Vec3* mn, Vec3* mx) {
    if (!a || !mn || !mx) return false;
    Vec3 pos; AABB bb;
    if (!sehGetPosBox(a, &pos, &bb)) return false;
    Vec3 dim = clampActorDim(bb);
    float footY = bb.lower.y;
    if (!(footY < pos.y + 0.5f) || footY < pos.y - 4.0f) footY = pos.y - dim.y * 0.5f;
    mn->x = pos.x - dim.x * 0.5f; mn->y = footY;             mn->z = pos.z - dim.z * 0.5f;
    mx->x = pos.x + dim.x * 0.5f; mx->y = footY + dim.y;     mx->z = pos.z + dim.z * 0.5f;
    return true;
}

// Enumerate every remote player (isPlayer + not the local player), invoking
// cb(Actor*) for each. Returns the number of players visited, or -1 if the
// actor list could not be fetched. Callback signature: void(Actor*).
template <typename Fn>
inline int forEachPlayer(const GameContext& ctx, Fn&& cb) {
    if (!ctx.level) return -1;
    std::vector<Actor*> actors;
    if (!sehGetActorList(ctx.level, &actors)) return -1;
    int n = 0;
    for (Actor* a : actors) {
        if (!a || a == ctx.lp) continue;
        if (!sehIsPlayer(a)) continue;
        cb(a);
        ++n;
    }
    return n;
}

// SEH-guarded Level::getPlayerList. The map holds EVERY connected player (not
// just nearby actors), which is what TabList and AntiBot enumerate.
inline std::unordered_map<mcUUID, PlayerListEntry, mcUUIDHasher>* sehGetPlayerList(Level* level) {
    if (!level) return nullptr;
    __try { return level->getPlayerList(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// One PlayerListEntry's name. Returns false when the string is unreadable or
// empty. sdk_safe_string copies through a POD buffer, so this stays SEH-legal.
inline bool sehGetPlayerListName(PlayerListEntry* entry, std::string* out) {
    if (!entry || !out) return false;
    if (!sdk_safe_string(&entry->name, *out)) return false;
    return !out->empty();
}

// An actor's raw (server) name. Deliberately NOT inside a __try: getRawName()
// returns a std::string, and a function that unwinds C++ objects cannot host
// SEH (MSVC C2712). getRawName() is internally SEH-guarded, so this is safe.
inline bool sehGetName(Actor* a, std::string* out) {
    if (!a || !out) return false;
    *out = a->getRawName();
    return !out->empty();
}

// Actor health, or -1 when unavailable. getHealth() validates every pointer via
// sdk_safe_ptr internally, so no __try is needed (and none is possible: it
// returns a std::optional).
inline float sehGetHealth(Actor* a) {
    if (!a) return -1.0f;
    auto h = a->getHealth();
    return h.has_value() ? *h : -1.0f;
}

// A point on an actor's hitbox to aim at: 0 legs, 1 centre, 2 head. Centred
// in x/z, fixed fraction of the box height in y.
inline Vec3 aimPointOnBox(const Vec3& mn, const Vec3& mx, int aimPoint) {
    Vec3 aim;
    aim.x = (mn.x + mx.x) * 0.5f;
    aim.z = (mn.z + mx.z) * 0.5f;
    float height = mx.y - mn.y;
    if (height < 0.1f) height = 1.8f;
    switch (aimPoint) {
        case 0: aim.y = mn.y + height * 0.28f; break;  // legs (thighs)
        case 2: aim.y = mn.y + height * 0.92f; break;  // head (eye line)
        default: aim.y = mn.y + height * 0.58f; break; // centre (chest)
    }
    return aim;
}

} // namespace SDK
