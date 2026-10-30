#pragma once
#include "BaritoneCore.h"

#include <memory>
#include <unordered_map>

namespace baritone {

class Pathfinder {
public:
    void begin(const BlockPos& startPos, std::shared_ptr<Goal> newGoal, const PathOptions& newOptions);
    SearchStatus step(const IWorld& world);
    void cancel();

    [[nodiscard]] SearchStatus getStatus() const { return status; }
    [[nodiscard]] std::size_t getExpandedNodeCount() const { return expanded; }
    [[nodiscard]] const std::vector<PathNode>& getPath() const { return result; }
    [[nodiscard]] std::vector<PathNode> getBestPathSoFar() const;

private:
    struct Node {
        double g = 1e18;
        double f = 0.0;
        BlockPos parent{};
        MovementType movement = MovementType::Start;
        double movementCost = 0.0;
        bool hasParent = false;
        bool closed = false;
    };

    struct QueueEntry {
        BlockPos pos;
        double f;
        double g;
    };

    struct QueueCompare {
        bool operator()(const QueueEntry& a, const QueueEntry& b) const { return a.f > b.f; }
    };

    void finish(SearchStatus newStatus, const BlockPos& end);
    std::vector<PathNode> reconstruct(const BlockPos& end) const;

    PathOptions options;
    std::shared_ptr<Goal> goal;
    BlockPos start;
    BlockPos best;
    BlockPos mostRecent;
    bool hasMostRecent = false;
    double bestHeuristic = 0.0;
    double bestScore = 0.0;
    std::size_t expanded = 0;
    SearchStatus status = SearchStatus::Cancelled;

    std::vector<QueueEntry> open;
    std::vector<PathNode> result;
    std::unordered_map<BlockPos, Node, SDK::BlockPosHash> nodes;
    mutable std::unordered_map<BlockPos, BlockState, SDK::BlockPosHash> worldCache;
    std::vector<Movement> successorBuffer;

    class CachedWorld;
    friend class CachedWorld;
};

} // namespace baritone
