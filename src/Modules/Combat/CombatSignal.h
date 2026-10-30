#pragma once
#include <chrono>

// ===========================================================================
// CombatSignal — a tiny, dependency-free "last hit" broadcast.
//
// Sunstead has no centralized event dispatch like Latite's AttackEvent system,
// so combat modules can't listen for hits. This header provides a shared signal
// the Aura module raises on every successful GameMode::attack, and that HUD
// modules (TargetHUD) poll to drive hit flashes / particle bursts.
//
// Edge detection: consumers compare g_hitCount against a locally-cached copy;
// a mismatch means "one or more new hits happened since I last looked". This is
// robust regardless of frame rate (unlike a raw time-since-hit threshold).
//
// Header-only inline globals (C++17). No locking: writes happen on the game
// thread (Aura::OnUpdate) and reads on the render thread; a torn read of a
// 64-bit counter at worst delays a single flash by one frame, which is benign.
// ===========================================================================
namespace CombatSignal {
    inline unsigned long long g_hitCount = 0;      // bumped once per landed hit
    inline const void*        g_lastTarget = nullptr; // most recently hit actor
    inline std::chrono::steady_clock::time_point g_lastHit{};

    inline void registerHit(const void* target) {
        ++g_hitCount;
        g_lastTarget = target;
        g_lastHit = std::chrono::steady_clock::now();
    }

    // Milliseconds since the last landed hit (huge if none yet).
    inline double msSinceLastHit() {
        if (g_lastHit.time_since_epoch().count() == 0) return 1e9;
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - g_lastHit).count();
    }
}
