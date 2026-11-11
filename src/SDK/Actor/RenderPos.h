#pragma once
#include "Actor.h"
#include "../Types.h"
#include "../Client/ClientInstance.h"
#include "../Client/GameContext.h"
#include <Windows.h>
#include <unordered_map>
#include <cmath>

extern volatile float g_servedFrameAlpha;
extern volatile long  g_qTail;

unsigned ServedPushSerial();

bool ServedSelfPair(Vec3* pos, Vec3* posOld);

namespace SDK {

inline bool sehReadPosPair(Actor* a, Vec3* posOut, Vec3* oldOut) {
    __try {
        *posOut = a->getPos();
        *oldOut = a->getPosOld();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline float sehReadTimerAlphaNow() {
    __try {
        ClientInstance* ci = ClientInstance::get();
        if (!ci || !sdk_safe_ptr(ci, sizeof(void*))) return -1.0f;
        Minecraft* mc = ci->getMinecraft();
        if (!mc || !sdk_safe_ptr(mc, sizeof(void*))) return -1.0f;
        Timer* tm = mc->getTimer();
        if (!tm || !sdk_safe_ptr(tm, sizeof(Timer))) return -1.0f;
        float a = tm->alpha;
        float tps = tm->tps;
        if (!(tps > 5.0f && tps < 120.0f)) return -1.0f;
        if (!(std::isfinite(a) && a >= 0.0f && a <= 1.0f)) return -1.0f;
        return a;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1.0f; }
}

namespace PushPairRing {
    struct Pair { Vec3 pos, old; bool valid; };
    struct Snap {
        unsigned serial = 0xFFFFFFFFu;
        volatile long ready = 0;
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

inline void CapturePushPairs(Level* level) {
    if (!level || !sdk_safe_ptr(level, sizeof(void*))) return;

    PushPairRing::Snap& snap = PushPairRing::ring[PushPairRing::tail % PushPairRing::kDepth];
    PushPairRing::tail = PushPairRing::tail + 1;
    snap.ready = 0;
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
    const long t = g_qTail;
    snap.serial = (unsigned)(t > 0 ? t - 1 : 0);
    InterlockedExchange(&snap.ready, 1);
}

inline Vec3 interpRenderPos(Actor* a) {
    if (!a) return Vec3(0, 0, 0);

    const float alpha = g_servedFrameAlpha;
    if (std::isfinite(alpha) && alpha >= 0.0f && alpha <= 1.0f) {
        const PushPairRing::Snap* snap = PushPairRing::find(ServedPushSerial());
        if (snap) {
            auto it = snap->pairs.find((void*)a);
            if (it != snap->pairs.end() && it->second.valid) {
                const Vec3& sPos = it->second.pos;
                const Vec3& sOld = it->second.old;
                const float sdx = sPos.x - sOld.x, sdy = sPos.y - sOld.y, sdz = sPos.z - sOld.z;
                if (sdx * sdx + sdy * sdy + sdz * sdz > 64.0f) return sPos;
                return Vec3(sOld.x + sdx * alpha,
                            sOld.y + sdy * alpha,
                            sOld.z + sdz * alpha);
            }
        }
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
