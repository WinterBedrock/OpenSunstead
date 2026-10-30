#pragma once
// Shared render-position interpolation for world-space overlays (ESP, NameTags,
// Tracers). The game draws each model at lerp(posOld, pos, Timer::alpha); reading
// raw pos instead gives the "box snaps after the player" jitter.
//
// TIMING RULE: the pos-pair AND the alpha must describe the SAME instant. Pairs
// are snapshotted at PUSH time (CapturePushPairs, right after writeMatrices) and
// lerped with the served push's alpha (g_servedFrameAlpha) and its A/B-verified
// delay-1 camera matrix, so pair, alpha and matrix all describe the exact frame
// the presented world was drawn from.
#include "Actor.h"
#include "../Types.h"
#include "../Client/ClientInstance.h"
#include "../Client/GameContext.h"
#include <Windows.h>
#include <unordered_map>
#include <cmath>

// Defined in GameHooks.cpp / declared in Render/FrameView.h:
//   g_servedFrameAlpha — Timer::alpha OF THE SERVED PUSH
//   g_qTail            — total camera pushes (newest serial = tail-1)
extern volatile float g_servedFrameAlpha;
extern volatile long  g_qTail;

// Serial of the SERVED push (defined in GameHooks.cpp). interpRenderPos uses
// it to pick the per-push pair snapshot (PushPairRing below).
unsigned ServedPushSerial();

// The SERVED push's local-player anchor (defined in GameHooks.cpp).
bool ServedSelfPair(Vec3* pos, Vec3* posOld);

namespace SDK {

// SEH-isolated pos pair read (an actor pointer can go stale mid-frame; the
// __try frame must stay free of C++ objects, hence the POD out-params).
inline bool sehReadPosPair(Actor* a, Vec3* posOut, Vec3* oldOut) {
    __try {
        *posOut = a->getPos();
        *oldOut = a->getPosOld();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// SEH-isolated live Timer::alpha read. Returns -1 when unavailable. Used as
// the fallback when the camera queue has not served a frame (menus).
inline float sehReadTimerAlphaNow() {
    __try {
        ClientInstance* ci = ClientInstance::get();
        if (!ci || !sdk_safe_ptr(ci, sizeof(void*))) return -1.0f;
        Minecraft* mc = ci->getMinecraft();
        if (!mc || !sdk_safe_ptr(mc, sizeof(void*))) return -1.0f;
        Timer* tm = mc->getTimer();
        if (!tm || !sdk_safe_ptr(tm, sizeof(Timer))) return -1.0f;
        float a = tm->alpha;
        // Sanity: the struct's tps field must look like a tick rate, else the
        // Timer layout shifted on a game update and alpha is garbage.
        float tps = tm->tps;
        if (!(tps > 5.0f && tps < 120.0f)) return -1.0f;
        if (!(std::isfinite(a) && a >= 0.0f && a <= 1.0f)) return -1.0f;
        return a;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1.0f; }
}

// Per-push pair snapshots — the actor-side twin of the camera queue.
// CapturePushPairs() runs per PUSHED camera transform (GameHooks, render
// thread, right after writeMatrices) and stores every actor's posOld/pos under
// the push's serial; interpRenderPos lerps the snapshot matching the SERVED
// push's serial with that push's alpha — exact by construction. On a miss
// (new actor, world transition, empty queue) it falls back to the LIVE pair
// + LIVE alpha (self-consistent, approximate frame lock).
namespace PushPairRing {
    struct Pair { Vec3 pos, old; bool valid; };
    struct Snap {
        unsigned serial = 0xFFFFFFFFu;   // published LAST (release ordering)
        volatile long ready = 0;         // 1 once serial is valid
        std::unordered_map<void*, Pair> pairs;
    };
    constexpr int kDepth = 8;
    inline Snap ring[kDepth];
    inline long tail = 0;

    inline const Snap* find(unsigned serial) {
        if (serial == 0xFFFFFFFFu) return nullptr;
        for (int i = 0; i < kDepth; ++i) {
            Snap& s = ring[i];
            if (s.ready != 0 && s.serial == serial) return &s;
        }
        return nullptr;
    }
}

// Called from GameHooks right after a camera transform was pushed. No outer
// __try (the C++ containers live here); raw access goes through the POD SEH
// helpers, so a stale actor mid-iteration just skips.
inline void CapturePushPairs(Level* level) {
    if (!level || !sdk_safe_ptr(level, sizeof(void*))) return;

    PushPairRing::Snap& snap = PushPairRing::ring[PushPairRing::tail % PushPairRing::kDepth];
    PushPairRing::tail = PushPairRing::tail + 1;
    snap.ready = 0;                       // close the slot while rewriting it
    snap.serial = 0xFFFFFFFFu;
    snap.pairs.clear();

    std::vector<Actor*> actors;
    if (!sehGetActorList(level, &actors)) return;
    snap.pairs.reserve(actors.size());
    for (Actor* a : actors) {
        Vec3 pos(0, 0, 0), old(0, 0, 0);
        if (!sehReadPosPair(a, &pos, &old)) continue;
        PushPairRing::Pair p = { pos, old, true };
        snap.pairs[(void*)a] = p;
    }
    // The serial of the push that JUST landed (g_qTail was already advanced
    // by pushCameraTransform, so the newest push is tail-1).
    const long t = g_qTail;
    snap.serial = (unsigned)(t > 0 ? t - 1 : 0);
    InterlockedExchange(&snap.ready, 1);
}

// The position the presented frame's model was drawn at: snapshot hit ->
// lerp(pair@push, alpha@push), exact; miss -> LIVE pair + LIVE alpha
// (approximate frame lock); no Timer -> wall-clock tick-fraction fallback.
inline Vec3 interpRenderPos(Actor* a) {
    if (!a) return Vec3(0, 0, 0);

    const float alpha = g_servedFrameAlpha;
    if (std::isfinite(alpha) && alpha >= 0.0f && alpha <= 1.0f) {
        // The EXACT pair of the served push, when the ring has it.
        const PushPairRing::Snap* snap = PushPairRing::find(ServedPushSerial());
        if (snap) {
            auto it = snap->pairs.find((void*)a);
            if (it != snap->pairs.end() && it->second.valid) {
                const Vec3& sPos = it->second.pos;
                const Vec3& sOld = it->second.old;
                const float sdx = sPos.x - sOld.x, sdy = sPos.y - sOld.y, sdz = sPos.z - sOld.z;
                // Teleport / respawn: snap (no interpolation) to avoid a slide.
                if (sdx * sdx + sdy * sdy + sdz * sdz > 64.0f) return sPos;
                return Vec3(sOld.x + sdx * alpha,
                            sOld.y + sdy * alpha,
                            sOld.z + sdz * alpha);
            }
        }
        // Snapshot miss: live pair + live alpha — same-instant pair/alpha.
        Vec3 pos(0, 0, 0), old(0, 0, 0);
        if (sehReadPosPair(a, &pos, &old)) {
            const float dx = pos.x - old.x, dy = pos.y - old.y, dz = pos.z - old.z;
            if (dx * dx + dy * dy + dz * dz > 64.0f) return pos;   // teleport
            float liveAlpha = sehReadTimerAlphaNow();
            if (liveAlpha >= 0.0f)
                return Vec3(old.x + dx * liveAlpha, old.y + dy * liveAlpha, old.z + dz * liveAlpha);
        }
    }

    // Last resort: the wall-clock reconstruction (menus / queue empty).
    Vec3 pos(0, 0, 0), old(0, 0, 0);
    if (!sehReadPosPair(a, &pos, &old)) return Vec3(0, 0, 0);
    float dx = pos.x - old.x, dy = pos.y - old.y, dz = pos.z - old.z;
    if (dx * dx + dy * dy + dz * dz > 64.0f) return pos;

    float liveAlpha = sehReadTimerAlphaNow();
    if (liveAlpha >= 0.0f)
        return Vec3(old.x + dx * liveAlpha, old.y + dy * liveAlpha, old.z + dz * liveAlpha);

    struct Tick { Vec3 lastPos; float tMs; };
    static std::unordered_map<void*, Tick> s_tick;
    float now = (float)GetTickCount64();
    auto it = s_tick.find(a);
    if (it == s_tick.end()) {
        s_tick[a] = { pos, now };
        return pos;
    }
    Tick& tk = it->second;
    if (pos.x != tk.lastPos.x || pos.y != tk.lastPos.y || pos.z != tk.lastPos.z) {
        tk.lastPos = pos; tk.tMs = now;   // new tick boundary
    }
    float walpha = (now - tk.tMs) / 50.0f; // 50ms = one 20Hz tick
    if (walpha < 0.0f) walpha = 0.0f;
    if (walpha > 1.0f) walpha = 1.0f;
    return Vec3(old.x + dx * walpha, old.y + dy * walpha, old.z + dz * walpha);
}

// interpSelfRenderPos — the LOCAL player's render position: the SERVED push's
// snapshot lerped with its alpha, exact by construction. Fallback: the served
// push's captured pair (ServedSelfPair), then interpRenderPos's live machinery.
inline Vec3 interpSelfRenderPos(Actor* lp) {
    if (!lp) return Vec3(0, 0, 0);
    {
        Vec3 snap = interpRenderPos(lp);
        if (snap.x != 0.0f || snap.y != 0.0f || snap.z != 0.0f) return snap;
    }
    Vec3 pos(0, 0, 0), old(0, 0, 0);
    if (::ServedSelfPair(&pos, &old)) {
        float dx = pos.x - old.x, dy = pos.y - old.y, dz = pos.z - old.z;
        // Teleport / respawn: snap.
        if (dx * dx + dy * dy + dz * dz > 64.0f) return pos;
        const float alpha = g_servedFrameAlpha;
        if (std::isfinite(alpha) && alpha >= 0.0f && alpha <= 1.0f)
            return Vec3(old.x + dx * alpha, old.y + dy * alpha, old.z + dz * alpha);
        return pos;
    }
    return interpRenderPos(lp);
}

} // namespace SDK
