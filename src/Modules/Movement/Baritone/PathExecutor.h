#pragma once
#include "BaritoneCore.h"

#include <cstddef>

namespace SDK { class Actor; }

namespace baritone {

class BlockSourceWorld;

enum class ExecutionStatus {
    Running,
    Arrived,
    NoPlayer,
    Stuck,
    OffPath,
};

struct ExecutionOptions {
    bool sprint = true;
    bool faceTravel = true;
};

// Follows a movement-typed path by driving Bedrock's own MoveInputComponent:
// analog stick plus coherent direction bits in both input states. Never
// writes velocity or calls jumpFromGround — the client's movement systems
// stay authoritative, which keeps BDS movement checks happy.
class PathExecutor {
public:
    void begin(std::vector<PathNode> newPath);
    void stop(SDK::Actor* player);
    ExecutionStatus tick(SDK::Actor* player, const BlockSourceWorld& world, const ExecutionOptions& options);

    // Recovery hop: hold jump for a short window. Clears sub-block lips the
    // whole-block search rounded off, before the controller resorts to a replan.
    void requestHop() { hopUntilMs = nowMs() + 600; }
    [[nodiscard]] bool hopActive() const { return nowMs() < hopUntilMs; }

    [[nodiscard]] std::size_t getCurrentIndex() const { return index; }
    [[nodiscard]] const std::vector<PathNode>& getPath() const { return path; }
    [[nodiscard]] const char* getLastFailureReason() const { return lastFailureReason; }

    // Silent-rotation channel, consumed by the module's packet hook.
    [[nodiscard]] bool consumeTravelYaw(float& outYaw) const {
        if (!travelYawActive) return false;
        outYaw = travelYaw;
        return true;
    }

private:
    void clearInput(SDK::Actor* player);
    void resetParkourState();
    void resetAscendState();
    void writeInput(SDK::Actor* player, float forwardAmount, float strafeAmount,
                    bool sprint, bool jump, bool sneak);
    static unsigned long long nowMs();

    std::vector<PathNode> path;
    std::size_t index = 0;
    const char* lastFailureReason = "";
    unsigned long long hopUntilMs = 0;

    // progress tracking
    Vec3 lastProgressPosition{};
    Vec3 lastMotionPosition{};
    bool progressInitialized = false;
    bool motionSampleInitialized = false;
    int ticksWithoutProgress = 0;
    int ticksOutsidePath = 0;

    // ascend state
    std::size_t activeAscendIndex = static_cast<std::size_t>(-1);
    bool ascendJumpIssued = false;
    bool ascendWasAirborne = false;
    int ascendLaunchTicks = 0;
    int ascendRetries = 0;
    int ascendRetryDelay = 0;

    // parkour state
    std::size_t activeParkourIndex = static_cast<std::size_t>(-1);
    bool parkourJumpIssued = false;
    bool parkourWasAirborne = false;
    bool parkourRepositioning = false;
    bool parkourRunwayPrepared = false;
    bool parkourAirControlActive = false;
    int parkourSprintTicks = 0;
    int parkourLaunchTicks = 0;

    // travel facing published for the packet hook
    float travelYaw = 0.f;
    mutable bool travelYawActive = false;
};

} // namespace baritone
