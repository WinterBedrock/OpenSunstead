#pragma once
// Sunstead SDK — shared plumbing (time, rng, focus, screen guards, throttled
// logging). Layout rule: POD-only inside every __try (MSVC C2712 forbids __try
// in a frame that needs C++ unwinding); std::string building goes through a POD
// helper first.

#include "../Types.h"
#include "../Client/ScreenView.h"
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <Windows.h>

namespace SDK {

// Time — all helpers share one clock (steady_clock). Comparing values from
// different clocks (steady_clock ms vs GetTickCount64 vs QPC) is silently wrong.
inline uint64_t nowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline double nowSeconds() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Sub-millisecond monotonic clock (QueryPerformanceCounter) in milliseconds.
inline double nowMsHiRes() {
    static LARGE_INTEGER s_freq{};
    if (s_freq.QuadPart == 0) QueryPerformanceFrequency(&s_freq);
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return s_freq.QuadPart ? (double)c.QuadPart * 1000.0 / (double)s_freq.QuadPart : 0.0;
}

// Randomness — one engine, seeded once. Avoid clock-derived "random" such as
// GetTickCount64() % range: correlated with a clock the caller compares against,
// it moves in lockstep with the throttle.
inline unsigned& rngState() {
    static unsigned s = 0;
    if (s == 0) {
        s = (unsigned)(nowMs() ^ (uintptr_t)&s ^ GetCurrentThreadId());
        if (s == 0) s = 0x9E3779B9u;
    }
    return s;
}

// xorshift32 — deterministic, fast, no global lock, plenty for jitter.
inline unsigned rngNext() {
    unsigned& s = rngState();
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

inline float randUnit() { return (float)(rngNext() >> 8) / (float)(1u << 24); } // [0,1)
inline float randRange(float lo, float hi) { return lo + randUnit() * (hi - lo); }
inline float randSigned(float mag) { return (randUnit() * 2.0f - 1.0f) * mag; }
inline int   randInt(int lo, int hi) { // inclusive
    if (hi <= lo) return lo;
    return lo + (int)(rngNext() % (unsigned)(hi - lo + 1));
}

// Window focus
inline bool isGameWindowFocused() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// Screen guards
inline bool isHudScreen() {
    return ScreenView::currentScreenName() == "hud_screen";
}

// "Draw/act only in world, unless the module's own Always-Show toggle is set."
inline bool onlyOnHud(bool alwaysShow) { return alwaysShow || isHudScreen(); }

// Throttled debug logging. Throttle state is per call site (an explicit
// `state` reference; a shared timestamp would let two same-interval modules
// starve each other). Formats into a caller-provided buffer so the varargs
// frame stays POD; the caller does the Logger call, so this header stays
// Logger-free and LOG_INFO can compile out in release, literal included.
struct DbgThrottle { uint64_t last = 0; };

// va_list form — the one that owns the throttle + formatting logic.
inline bool dbgFormatV(DbgThrottle& state, bool enabled, unsigned intervalMs,
                       char* buf, size_t bufSize, const char* fmt, va_list ap) {
    if (!enabled || !buf || bufSize == 0) return false;
    const uint64_t now = nowMs();
    if (intervalMs && (now - state.last) < intervalMs) return false;
    state.last = now;
    _vsnprintf_s(buf, bufSize, _TRUNCATE, fmt, ap);
    return true;
}

inline bool dbgFormat(DbgThrottle& state, bool enabled, unsigned intervalMs,
                      char* buf, size_t bufSize, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const bool ok = dbgFormatV(state, enabled, intervalMs, buf, bufSize, fmt, ap);
    va_end(ap);
    return ok;
}

// Declares a module-local `name(bool enabled, const char* fmt, ...)` that logs
// through the shared throttle. In release the format literal compiles out with
// LOG_INFO (it only ever reaches the logger through this macro).
#define SUNSTEAD_DEFINE_DBG(name, tag, intervalMs)                              \
    static ::SDK::DbgThrottle name##_state;                                     \
    static void name(bool enabled, const char* fmt, ...) {                      \
        char buf[256];                                                          \
        va_list ap;                                                             \
        va_start(ap, fmt);                                                      \
        const bool ok = ::SDK::dbgFormatV(name##_state, enabled, (intervalMs),   \
                                          buf, sizeof(buf), fmt, ap);           \
        va_end(ap);                                                             \
        if (ok) LOG_INFO(std::string(buf), tag);                                \
    }

} // namespace SDK
