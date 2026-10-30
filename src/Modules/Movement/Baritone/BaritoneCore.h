#pragma once
#include "../../../SDK/World/World.h"
#include "../../../SDK/Types.h"
#include <cmath>
#include <cstdint>
#include <vector>

namespace baritone {

using BlockPos = SDK::BlockPos;

enum class MovementType : uint8_t {
    Start,
    Traverse,
    Diagonal,
    Ascend,
    Descend,
    Fall,
    Parkour,
    Swim,
    WaterDrop,
};

struct Movement {
    BlockPos destination;
    MovementType type;
    double cost;
};

struct PathNode {
    BlockPos pos;
    MovementType movement = MovementType::Start;
    double costFromPrevious = 0.0;
};

enum class SearchStatus { Searching, Found, Partial, Failed, Cancelled };

struct BlockState {
    bool loaded = false;
    bool solid = false;
    bool liquid = false;
    bool hazard = false;
};

class IWorld {
public:
    virtual ~IWorld() = default;
    [[nodiscard]] virtual BlockState getBlock(const BlockPos& pos) const = 0;
};

struct PathOptions {
    bool allowDiagonal = true;
    bool allowAscend = true;
    bool allowFall = true;
    bool allowWater = false;
    bool allowParkour = false;
    int maxFallHeight = 3;
    int maxParkourDistance = 3;
    std::size_t maxExpandedNodes = 60000;
    std::size_t nodesPerStep = 4000;
    double heuristicWeight = 1.1;
    bool preferSprint = true;
};

namespace action_costs {
    constexpr double costInf = 1e9;
    constexpr double walkOneBlock = 20.0;
    constexpr double sprintOneBlock = 14.0;
    constexpr double walkInWater = 70.0;
    constexpr double walkOffBlock = 22.0;
    constexpr double centerAfterFall = 30.0;
    constexpr double jumpOneBlock = 50.0;
    constexpr double parkourSetUp = 20.0;

    constexpr double fallTicks(const int height) {
        return height <= 1 ? 10.0 : 10.0 + (height - 1) * 6.0;
    }
    constexpr double parkourJump(const int distance, const bool sprint) {
        return (sprint ? sprintOneBlock : walkOneBlock) * distance + parkourSetUp + 40.0;
    }
} // namespace action_costs

class Goal {
public:
    virtual ~Goal() = default;
    [[nodiscard]] virtual bool isInGoal(const BlockPos& pos) const = 0;
    [[nodiscard]] virtual double heuristic(const BlockPos& pos) const = 0;
};

class GoalBlock final : public Goal {
    BlockPos target;

public:
    explicit GoalBlock(const BlockPos& pos) : target(pos) {}
    [[nodiscard]] bool isInGoal(const BlockPos& pos) const override {
        return pos.x == target.x && pos.y == target.y && pos.z == target.z;
    }
    [[nodiscard]] double heuristic(const BlockPos& pos) const override {
        const double dx = std::abs(pos.x - target.x);
        const double dy = std::abs(pos.y - target.y);
        const double dz = std::abs(pos.z - target.z);
        return (std::max(dx, dz) - std::min(dx, dz)) + 1.41421356 * std::min(dx, dz) + dy;
    }
};

class GoalNear final : public Goal {
    BlockPos target;
    int range;

public:
    GoalNear(const BlockPos& pos, const int blocks) : target(pos), range(blocks) {}
    [[nodiscard]] bool isInGoal(const BlockPos& pos) const override {
        return std::abs(pos.x - target.x) + std::abs(pos.y - target.y) +
               std::abs(pos.z - target.z) <= range;
    }
    [[nodiscard]] double heuristic(const BlockPos& pos) const override {
        const double dx = std::abs(pos.x - target.x);
        const double dy = std::abs(pos.y - target.y);
        const double dz = std::abs(pos.z - target.z);
        const double manhattan = dx + dy + dz - range;
        return std::max(0.0, (manhattan - std::min(dx, dz)) * 0.41421356 + manhattan);
    }
};

class GoalXZ final : public Goal {
    int x, z;

public:
    GoalXZ(const int targetX, const int targetZ) : x(targetX), z(targetZ) {}
    [[nodiscard]] bool isInGoal(const BlockPos& pos) const override {
        return pos.x == x && pos.z == z;
    }
    [[nodiscard]] double heuristic(const BlockPos& pos) const override {
        const double dx = std::abs(pos.x - x);
        const double dz = std::abs(pos.z - z);
        return (std::max(dx, dz) - std::min(dx, dz)) + 1.41421356 * std::min(dx, dz);
    }
};

struct MovementGenerator {
    static bool canOccupy(const IWorld& world, const BlockPos& pos, const PathOptions& options);
    static bool canStandAt(const IWorld& world, const BlockPos& pos, const PathOptions& options);
    static bool canDropToWater(const IWorld& world, const BlockPos& from, const BlockPos& to);
    static void getMovements(const IWorld& world, const BlockPos& from,
                              const PathOptions& options, std::vector<Movement>& result);
};

} // namespace baritone
