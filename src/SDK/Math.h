#pragma once
// Sunstead SDK — shared math layer (vector / angle / matrix / worldToScreen).
// The geometry PODs (Vec2/Vec3/Matrix4/AABB) live in Types.h; this header only
// adds free operators/functions over them, so it must come AFTER Types.h.

#include <cmath>
#include <imgui.h>

namespace SDKMath {
    constexpr float PI  = 3.14159265358979323846f;
    constexpr float D2R = 0.01745329251994329577f; // degrees -> radians
    constexpr float R2D = 57.2957795130823208768f; // radians -> degrees
}

// Vec2
inline Vec2 operator+(const Vec2& a, const Vec2& b) { return Vec2(a.x + b.x, a.y + b.y); }
inline Vec2 operator-(const Vec2& a, const Vec2& b) { return Vec2(a.x - b.x, a.y - b.y); }
inline Vec2 operator*(const Vec2& a, float s)        { return Vec2(a.x * s, a.y * s); }
inline Vec2 operator/(const Vec2& a, float s)        { return Vec2(a.x / s, a.y / s); }
inline Vec2& operator*=(Vec2& a, float s)            { a.x *= s; a.y *= s; return a; }
inline Vec2& operator/=(Vec2& a, float s)            { a.x /= s; a.y /= s; return a; }
inline float vecDot(const Vec2& a, const Vec2& b)    { return a.x * b.x + a.y * b.y; }
inline float vecLengthSq(const Vec2& a)              { return a.x * a.x + a.y * a.y; }
inline float vecLength(const Vec2& a)                { return std::sqrt(vecLengthSq(a)); }
inline Vec2  vecNormalize(const Vec2& a)              { float l = vecLength(a); return (l > 1e-6f) ? Vec2(a.x / l, a.y / l) : Vec2(0, 0); }

// Vec3 — member operator+/- are in Types.h; these are the remaining helpers.
inline Vec3 operator*(const Vec3& a, float s)        { return Vec3(a.x * s, a.y * s, a.z * s); }
inline Vec3 operator/(const Vec3& a, float s)        { return Vec3(a.x / s, a.y / s, a.z / s); }

inline float vecDot(const Vec3& a, const Vec3& b)    { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3  vecCross(const Vec3& a, const Vec3& b)  { return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline float vecLengthSq(const Vec3& a)              { return a.x * a.x + a.y * a.y + a.z * a.z; }
inline float vecLength(const Vec3& a)                { return std::sqrt(vecLengthSq(a)); }
inline float vecDistanceSq(const Vec3& a, const Vec3& b) { float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z; return dx * dx + dy * dy + dz * dz; }
inline float vecDistance(const Vec3& a, const Vec3& b)   { return std::sqrt(vecDistanceSq(a, b)); }
inline Vec3  vecNormalize(const Vec3& a)             { float l = vecLength(a); return (l > 1e-6f) ? Vec3(a.x / l, a.y / l, a.z / l) : Vec3(0, 0, 0); }
// Horizontal (XZ-plane) distance — used for speed/reach checks that ignore Y.
inline float vecHorizontalDistance(const Vec3& a, const Vec3& b) { float dx = a.x - b.x, dz = a.z - b.z; return std::sqrt(dx * dx + dz * dz); }

// Angles (degrees)
// Wrap a raw yaw/pitch delta into [-180, 180] (the d-=360/d+=360 while-loop idiom).
inline float wrapAngle180(float deg) {
    while (deg >  180.0f) deg -= 360.0f;
    while (deg < -180.0f) deg += 360.0f;
    return deg;
}
// Shortest signed angular delta from `from` to `to`, both in degrees.
inline float angleDelta(float from, float to) { return wrapAngle180(to - from); }

// Look angles (degrees) from `eye` to `target`, Bedrock convention:
// yaw = atan2(dz,dx)*R2D - 90, pitch = -atan2(dy,horiz) clamped to +-89.9.
// Returns Vec2{pitch,yaw} — the ActorRotationComponent (x,y) ordering.
inline Vec2 calcLookAngles(const Vec3& eye, const Vec3& target) {
    float dx = target.x - eye.x;
    float dy = target.y - eye.y;
    float dz = target.z - eye.z;
    float horiz = std::sqrt(dx * dx + dz * dz);
    float yaw   = std::atan2(dz, dx) * SDKMath::R2D - 90.0f;
    float pitch = -std::atan2(dy, horiz) * SDKMath::R2D;
    if (pitch >  89.9f) pitch =  89.9f;
    if (pitch < -89.9f) pitch = -89.9f;
    yaw = wrapAngle180(yaw);
    return Vec2(pitch, yaw);
}

// Unit horizontal forward vector for a given yaw (degrees), Bedrock convention
// (forward = (-sin(yaw), 0, cos(yaw))). Y is always 0.
inline Vec3 forwardFromYaw(float yawDeg) {
    float r = yawDeg * SDKMath::D2R;
    return Vec3(-std::sin(r), 0.0f, std::cos(r));
}

// Matrix4 (column-major, element index = col*4 + row — the game's glm layout)
// C = A * B. Buffers may not alias the output.
inline void mat4Multiply(const float* A, const float* B, float* C) {
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += A[k * 4 + row] * B[col * 4 + k];
            C[col * 4 + row] = s;
        }
}

// World -> screen using a precomputed MVP (proj * view, column-major) and the
// render origin the matrices are built around. Mirrors Latite's
// WorldToScreen::convert. Returns false when behind the camera; `out` = pixels.
inline bool worldToScreen(const Vec3& world, const float* mvp, const Vec3& origin,
                          const ImVec2& screen, ImVec2* out) {
    float px = world.x - origin.x;
    float py = world.y - origin.y;
    float pz = world.z - origin.z;
    float cx = mvp[0] * px + mvp[4] * py + mvp[8]  * pz + mvp[12];
    float cy = mvp[1] * px + mvp[5] * py + mvp[9]  * pz + mvp[13];
    float cw = mvp[3] * px + mvp[7] * py + mvp[11] * pz + mvp[15];
    // Reject NaN/inf (bad MVP or position read) and points behind the near plane.
    if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(cw) || cw < 0.1f) return false;
    float inv = 1.0f / cw;
    out->x = (cx * inv + 1.0f) * 0.5f * screen.x;
    out->y = (1.0f - cy * inv) * 0.5f * screen.y;
    return std::isfinite(out->x) && std::isfinite(out->y);
}
