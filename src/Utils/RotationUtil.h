#pragma once

#include "../../SDK/Math.h"
#include <cmath>

namespace RotationUtil {

enum class SmoothMode { Linear = 0, EaseOut = 1 };

struct Params {
    // Degrees per second the head may travel (unsmoothed upper bound).
    float rotationSpeed = 620.0f;
    // Caps per-aim-step travel (deg). 0 = uncapped. Scaffold uses this so the
    // head never jumps >N deg even when a frame hitch inflates dt.
    float maxAnglePerTick = 0.0f;
    // Linear vs EaseOut — see step() doc.
    int smoothMode = (int)SmoothMode::Linear;
    // EaseOut curve exponent (<1 = steep start, >1 = gentle start). Only read
    // when smoothMode == EaseOut.
    float easeExponent = 0.5f;
    bool predictPlacementAngle = false;
    // Micro-jitter magnitude (deg). ±2.5 is within the Bedrock sensitivity
    // quantisation and believable for a human hand. set to 0 to disable.
    float microJitterDeg = 0.0f;
};

// Shortest-angle difference from `from` to `to` for yaw, direct diff for pitch.
inline float deltaDiff(float from, float to, bool isYaw) {
    return isYaw ? angleDelta(from, to) : (to - from);
}

// One smooth step. Returns the remaining angular error (deg), or the step
// magnitude the caller used.
inline float stepOnce(float cur, float target, float budgetDeg, const Params& p, bool isYaw) {
    const float fullDiff = deltaDiff(cur, target, isYaw);
    const float mag = std::fabs(fullDiff);
    if (mag < 1e-4f) return 0.0f;
    float travel = budgetDeg;
    if (p.smoothMode == (int)SmoothMode::EaseOut) {
        // EaseOut: larger fraction of mag when error is big, smaller when near.
        const float frac = std::pow(std::min(mag / (budgetDeg * 4.0f + 1.0f), 1.0f), p.easeExponent);
        travel = std::max(0.01f, travel * frac);
    }
    if (p.maxAnglePerTick > 0.0f)
        travel = std::min(travel, p.maxAnglePerTick);
    if (travel > mag) travel = mag;   // never overshoot
    return travel;
}

// Bias a raw aim target (eye->aim vector) for a placement surface
// (predictPlacementAngle). For pure-aim modules this is a no-op pass-through.
inline Vec3 adjustPlacementPredict(Vec3 eye, Vec3 aim, unsigned char face) {
    const float nx = (face == 4 ? -1.0f : face == 5 ? 1.0f : 0.0f);
    const float ny = (face == 0 ? -1.0f : face == 1 ? 1.0f : 0.0f);
    const float nz = (face == 2 ? -1.0f : face == 3 ? 1.0f : 0.0f);
    const float bias = 0.35f;
    return Vec3(aim.x + nx * bias, aim.y + ny * bias, aim.z + nz * bias);
}

} // namespace RotationUtil
