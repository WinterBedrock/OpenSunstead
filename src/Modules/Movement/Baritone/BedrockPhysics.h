#pragma once
#include <algorithm>
#include <cmath>

// Bedrock movement constants and small physics solvers. Positions and
// velocities are blocks / blocks-per-tick; the live StateVector stays the
// source of truth, these only predict takeoff and landing behaviour.
namespace baritone::bedrock_physics {

constexpr float gravity = 0.08f;
constexpr float verticalDrag = 0.98f;
constexpr float jumpVelocity = 0.42f;
constexpr float horizontalAirDrag = 0.91f;
constexpr float normalSlipperiness = 0.60f;
constexpr float walkGroundAcceleration = 0.098f;
constexpr float sprintGroundAcceleration = 0.1274f;
constexpr float sneakGroundAcceleration = 0.0294f;
constexpr float walkAirAcceleration = 0.0196f;
constexpr float sprintAirAcceleration = 0.02548f;
constexpr float sneakAirAcceleration = 0.00588f;
constexpr float playerHalfWidth = 0.30f;
constexpr float sneakEdgeMargin = 0.025f;
constexpr float safeTakeoffEdge = 0.5f + playerHalfWidth - sneakEdgeMargin;

inline float groundAcceleration(const bool sprinting, const bool sneaking) {
    if (sneaking)
        return sneakGroundAcceleration;
    return sprinting ? sprintGroundAcceleration : walkGroundAcceleration;
}

inline float nextGroundVelocity(const float alongVelocity, const float forwardInput,
                                const bool sprinting, const bool sneaking) {
    return alongVelocity * (horizontalAirDrag * normalSlipperiness) +
        std::clamp(forwardInput, -1.f, 1.f) * groundAcceleration(sprinting, sneaking);
}

inline float groundInputForTargetVelocity(const float alongVelocity,
                                          const float targetVelocity,
                                          const bool sprinting = false,
                                          const bool sneaking = false) {
    const float acceleration = groundAcceleration(sprinting, sneaking);
    if (!std::isfinite(alongVelocity) || !std::isfinite(targetVelocity) || acceleration <= 0.0001f)
        return 0.f;
    return std::clamp((targetVelocity - alongVelocity * horizontalAirDrag * normalSlipperiness) /
        acceleration, -1.f, 1.f);
}

inline float projectedAirDisplacement(float alongVelocity, const float forwardInput,
                                      const bool sprinting, const bool sneaking, const int ticks) {
    const float acceleration = sneaking ? sneakAirAcceleration :
        (sprinting ? sprintAirAcceleration : walkAirAcceleration);
    float displacement = 0.f;
    for (int tick = 0; tick < ticks; ++tick) {
        alongVelocity = alongVelocity * horizontalAirDrag +
            std::clamp(forwardInput, -1.f, 1.f) * acceleration;
        displacement += alongVelocity;
    }
    return displacement;
}

inline int ticksUntilLandingPlane(float y, float verticalVelocity, const float targetY,
                                  const int maximumTicks = 40) {
    bool descending = verticalVelocity <= 0.f;
    for (int tick = 1; tick <= maximumTicks; ++tick) {
        verticalVelocity = (verticalVelocity - gravity) * verticalDrag;
        y += verticalVelocity;
        descending = descending || verticalVelocity <= 0.f;
        if (descending && y <= targetY + 0.05f)
            return tick;
    }
    return maximumTicks;
}

inline bool holdJumpUntilAirborne(const bool jumpIssued, const bool wasAirborne,
                                   const bool grounded, const int launchUpdates) {
    // updatePlayer can run several times per 20 Hz movement tick; keep Space
    // down long enough for the native tick to consume it, bounded so a blocked
    // jump cannot stay latched forever.
    return jumpIssued && !wasAirborne && grounded && launchUpdates <= 10;
}

inline bool shouldRetryAscent(const bool grounded, const float feetY, const float landingY,
                              const bool wasAirborne, const int launchTicks) {
    // A head bump or landing on the near edge is not a failed jump; retry only
    // after settling back below the intended step.
    return grounded && feetY < landingY - 0.2f && (wasAirborne || launchTicks > 6);
}

inline bool parkourSprintReady(const int distance, const bool ascending, const bool sprintEnabled,
                               const int sprintTicks, const float measuredSpeed) {
    if (distance < 3 && !ascending)
        return measuredSpeed >= 0.09f;
    const float minimumSpeed = distance >= 4 ? 0.20f : 0.17f;
    return sprintEnabled && sprintTicks >= 2 && measuredSpeed >= minimumSpeed;
}

inline bool shouldBeginParkourAirControl(const int distance, const float progress,
                                         const float jumpLength,
                                         const float projectedWithForward,
                                         const float projectedCoasting) {
    const float minimumProgress = distance == 3 ? 0.20f : 0.35f;
    return progress > minimumProgress &&
        projectedWithForward >= jumpLength - 0.05f &&
        projectedCoasting >= jumpLength - 0.20f;
}

inline float waterDropAxisInput(const float error, const float velocity, const bool grounded) {
    if (grounded)
        return std::clamp(error * 0.5f - velocity * 4.f, -0.35f, 0.35f);
    const float desiredVelocity = std::clamp(error * 0.35f, -0.12f, 0.12f);
    return std::clamp((desiredVelocity - velocity * horizontalAirDrag) / walkAirAcceleration,
        -1.f, 1.f);
}

inline float fallLandingInput(const float progress, const float alongVelocity,
                              const float segmentLength, const int landingTicks,
                              const float targetProgress = 0.90f) {
    if (!std::isfinite(progress) || !std::isfinite(alongVelocity) ||
        segmentLength <= 0.001f || landingTicks <= 0)
        return 0.f;
    const float coast = projectedAirDisplacement(std::max(alongVelocity, 0.f), 0.f, false, false, landingTicks);
    const float forward = projectedAirDisplacement(std::max(alongVelocity, 0.f), 1.f, false, false, landingTicks);
    const float inputEffect = forward - coast;
    if (inputEffect <= 0.0001f)
        return 0.f;
    const float desired = (targetProgress - progress) * segmentLength;
    return std::clamp((desired - coast) / inputEffect, 0.f, 1.f);
}

} // namespace baritone::bedrock_physics
