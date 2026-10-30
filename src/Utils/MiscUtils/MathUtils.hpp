#pragma once
#include <cmath>
#include <algorithm>

namespace MathUtils {
    inline float lerp(float a, float b, float t) {
        return a + t * (b - a);
    }
    inline float animate(float target, float current, float speed) {
        if (speed < 0.0f) speed = 0.0f;
        else if (speed > 1.0f) speed = 1.0f;
        return lerp(current, target, speed);
    }
    inline float clamp(float value, float min, float max) {
        if (value < min) return min;
        if (value > max) return max;
        return value;
    }
    // Smoothstep (Hermite): slow start/end for UI expand animations.
    inline float smoothstep(float t) {
        t = clamp(t, 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }
    inline float easeOutCubic(float t) {
        t = clamp(t, 0.f, 1.f);
        float u = 1.f - t;
        return 1.f - u * u * u;
    }
    inline float easeInOutCubic(float t) {
        t = clamp(t, 0.f, 1.f);
        return t < 0.5f ? 4.f * t * t * t : 1.f - std::pow(-2.f * t + 2.f, 3.f) * 0.5f;
    }
    // Animate raw 0..1 then ease for layout (opening snappier, closing smoother).
    inline float animateEased(float target, float current, float dt, bool opening) {
        float speed = opening ? (dt * 10.5f) : (dt * 13.5f);
        float next = animate(target, current, speed);
        return next;
    }
}
