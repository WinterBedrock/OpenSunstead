#include "SilentRotation.h"
#include <atomic>
#include <cmath>

namespace {
    std::atomic<bool>  g_active{ false };
    std::atomic<bool>  g_once{ false };
    std::atomic<float> g_yaw{ 0.0f };
    std::atomic<float> g_pitch{ 0.0f };

    // Smoothing state, g_smooth is the per-Consume easing fraction 1 = snap
    std::atomic<float> g_smooth{ 1.0f };
    std::atomic<bool>  g_haveCur{ false };
    std::atomic<float> g_curYaw{ 0.0f };
    std::atomic<float> g_curPitch{ 0.0f };

    // Shortest signed angular difference b - a, wrapped to [-180,180]
    float angleDiff(float a, float b) {
        float d = b - a;
        while (d >  180.0f) d -= 360.0f;
        while (d < -180.0f) d += 360.0f;
        return d;
    }

    // Movement-frame restatement (see header). The client integrates WASD
    // against the REAL yaw; the server will integrate the same bits against
    // the SPOOFED yaw. We publish the live WASD state + real yaw here, and
    // the send hook re-expresses it in the spoofed frame.
    std::atomic<bool>  g_moveActive{ false };
    std::atomic<float> g_moveRealYaw{ 0.0f };
    std::atomic<int>   g_moveFwd{ 0 };
    std::atomic<int>   g_moveStrafe{ 0 };

    constexpr float kD2R = 3.141592653589793f / 180.0f;
}

namespace SilentRotation {

void SetContinuous(float yaw, float pitch) {
    g_yaw.store(yaw, std::memory_order_relaxed);
    g_pitch.store(pitch, std::memory_order_relaxed);
    g_once.store(false, std::memory_order_relaxed);
    g_active.store(true, std::memory_order_release);
}

void SetOnce(float yaw, float pitch) {
    g_yaw.store(yaw, std::memory_order_relaxed);
    g_pitch.store(pitch, std::memory_order_relaxed);
    g_once.store(true, std::memory_order_relaxed);
    g_active.store(true, std::memory_order_release);
}

void SetSmoothing(float factor) {
    if (factor < 0.02f) factor = 0.02f;
    if (factor > 1.0f)  factor = 1.0f;
    g_smooth.store(factor, std::memory_order_relaxed);
}

void Clear(bool force) {
    // A pending one-shot must survive a plain per-frame Clear so it can still be
    // injected into the next outgoing packet, only force clears everything
    if (force || !g_once.load(std::memory_order_acquire)) {
        g_active.store(false, std::memory_order_release);
        g_once.store(false, std::memory_order_release);
        // Drop the eased-rotation history so the next rotation starts from the current view, not the last eased result
        g_haveCur.store(false, std::memory_order_release);
    }
}

bool Consume(float& yaw, float& pitch) {
    if (!g_active.load(std::memory_order_acquire)) return false;
    float tYaw = g_yaw.load(std::memory_order_relaxed);
    float tPitch = g_pitch.load(std::memory_order_relaxed);

    bool once = g_once.load(std::memory_order_acquire);
    float f = g_smooth.load(std::memory_order_relaxed);

    if (once || f >= 0.999f || !g_haveCur.load(std::memory_order_acquire)) {
        yaw = tYaw; pitch = tPitch;
    } else {
        float cy = g_curYaw.load(std::memory_order_relaxed);
        float cp = g_curPitch.load(std::memory_order_relaxed);
        yaw   = cy + angleDiff(cy, tYaw) * f;   // shortest-angle lerp for yaw
        pitch = cp + (tPitch - cp) * f;          // pitch never wraps
    }

    // Remember the eased result for the next ticks continuation
    g_curYaw.store(yaw, std::memory_order_relaxed);
    g_curPitch.store(pitch, std::memory_order_relaxed);
    g_haveCur.store(true, std::memory_order_release);

    if (once) {
        g_once.store(false, std::memory_order_release);
        g_active.store(false, std::memory_order_release);
    }
    return true;
}

bool Peek(float& yaw, float& pitch) {
    if (!g_active.load(std::memory_order_acquire)) return false;
    yaw = g_yaw.load(std::memory_order_relaxed);
    pitch = g_pitch.load(std::memory_order_relaxed);
    return true;
}

void PublishMoveFrame(float realYaw) {
    g_moveRealYaw.store(realYaw, std::memory_order_relaxed);
    g_moveActive.store(true, std::memory_order_release);
}

void SetMoveInput(int fwd, int strafe) {
    g_moveFwd.store(fwd, std::memory_order_relaxed);
    g_moveStrafe.store(strafe, std::memory_order_relaxed);
}

bool ConsumeMoveFrame(float& forward, float& strafe) {
    if (!g_moveActive.load(std::memory_order_acquire)) return false;
    if (!g_active.load(std::memory_order_acquire)) return false;
    g_moveActive.store(false, std::memory_order_release);

    // World-space motion the client is about to produce: input direction
    // (fwd/strafe keys) rotated by the REAL yaw.
    const int fwd = g_moveFwd.load(std::memory_order_relaxed);
    const int str = g_moveStrafe.load(std::memory_order_relaxed);
    if (fwd == 0 && str == 0) return false; // standing still: nothing to restate
    const float realYaw = g_moveRealYaw.load(std::memory_order_relaxed);
    const float spoofYaw = g_yaw.load(std::memory_order_relaxed);

    const float ang = std::atan2((float)str, (float)fwd);
    const float worldAng = (realYaw + ang * 180.0f / 3.141592653589793f + 90.0f) * kD2R;
    const float wx = std::cos(worldAng);
    const float wz = std::sin(worldAng);

    // Re-express in the SPOOFED frame: forward/strafe the server must see so
    // its simulation lands on the same world-space motion.
    const float sy = spoofYaw * kD2R;
    const float fx = -std::sin(sy), fz = std::cos(sy); // spoofed forward
    const float sx = std::cos(sy),  sz = std::sin(sy); // spoofed right
    forward = wx * fx + wz * fz;
    strafe  = wx * sx + wz * sz;
    return true;
}

} // namespace SilentRotation
