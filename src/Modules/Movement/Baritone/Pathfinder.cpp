#include "Pathfinder.h"

#include <algorithm>
#include <chrono>
#include <queue>
#include <vector>

namespace baritone {

class Pathfinder::CachedWorld final : public IWorld {
    const IWorld& source;
    std::unordered_map<BlockPos, BlockState, SDK::BlockPosHash>& cache;

public:
    CachedWorld(const IWorld& src, std::unordered_map<BlockPos, BlockState, SDK::BlockPosHash>& c)
        : source(src), cache(c) {}

    [[nodiscard]] BlockState getBlock(const BlockPos& pos) const override {
        if (const auto found = cache.find(pos); found != cache.end())
            return found->second;
        const auto state = source.getBlock(pos);
        // Unloaded cells are not cached: the chunk may finish loading while the
        // multi-step search is still running.
        if (state.loaded)
            cache.emplace(pos, state);
        return state;
    }
};

void Pathfinder::begin(const BlockPos& startPos, std::shared_ptr<Goal> newGoal, const PathOptions& newOptions) {
    options = newOptions;
    goal = std::move(newGoal);
    start = startPos;
    best = startPos;
    mostRecent = startPos;
    hasMostRecent = false;
    expanded = 0;
    result.clear();
    nodes.clear();
    worldCache.clear();
    open.clear();
    successorBuffer.reserve(24);

    if (!goal) {
        status = SearchStatus::Failed;
        return;
    }

    bestHeuristic = goal->heuristic(start);
    bestScore = bestHeuristic;
    auto& startNode = nodes[start];
    startNode.g = 0.0;
    startNode.f = bestHeuristic * std::clamp(options.heuristicWeight, 1.0, 4.0);
    open.push_back({start, startNode.f, 0.0});
    std::make_heap(open.begin(), open.end(), QueueCompare{});
    status = SearchStatus::Searching;
}

void Pathfinder::cancel() {
    if (status == SearchStatus::Searching)
        status = SearchStatus::Cancelled;
    open.clear();
}

SearchStatus Pathfinder::step(const IWorld& world) {
    if (status != SearchStatus::Searching)
        return status;

    CachedWorld cachedWorld(world, worldCache);
    std::size_t processed = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);

    while (!open.empty() && processed < options.nodesPerStep) {
        if (processed > 0 && std::chrono::steady_clock::now() >= deadline)
            return status;

        std::pop_heap(open.begin(), open.end(), QueueCompare{});
        const auto entry = open.back();
        open.pop_back();

        auto it = nodes.find(entry.pos);
        if (it == nodes.end())
            continue;

        auto& current = it->second;
        if (current.closed || entry.g != current.g)
            continue;
        current.closed = true;
        mostRecent = entry.pos;
        hasMostRecent = true;
        ++processed;
        ++expanded;

        const double h = goal->heuristic(entry.pos);
        // Partial paths rank by goal progress first, cost only as a tie-breaker,
        // so expensive-but-correct lake crossings still beat cheap detours.
        const double score = h + current.g / 2.0;
        if (h < bestHeuristic || (h == bestHeuristic && score < bestScore)) {
            bestScore = score;
            bestHeuristic = h;
            best = entry.pos;
        }

        if (goal->isInGoal(entry.pos)) {
            finish(SearchStatus::Found, entry.pos);
            return status;
        }

        if (expanded >= options.maxExpandedNodes) {
            finish(best == start ? SearchStatus::Failed : SearchStatus::Partial, best);
            return status;
        }

        MovementGenerator::getMovements(cachedWorld, entry.pos, options, successorBuffer);
        for (const auto& movement : successorBuffer) {
            auto& next = nodes[movement.destination];
            if (next.closed)
                continue;

            const double candidate = current.g + movement.cost;
            if (candidate >= next.g)
                continue;

            next.g = candidate;
            next.f = candidate + goal->heuristic(movement.destination) *
                std::clamp(options.heuristicWeight, 1.0, 4.0);
            next.parent = entry.pos;
            next.hasParent = true;
            next.movement = movement.type;
            next.movementCost = movement.cost;
            open.push_back({movement.destination, next.f, candidate});
            std::push_heap(open.begin(), open.end(), QueueCompare{});
        }
    }

    if (open.empty())
        finish(best == start ? SearchStatus::Failed : SearchStatus::Partial, best);

    return status;
}

std::vector<PathNode> Pathfinder::getBestPathSoFar() const {
    return status == SearchStatus::Searching ? reconstruct(best) : std::vector<PathNode>{};
}

void Pathfinder::finish(const SearchStatus newStatus, const BlockPos& end) {
    status = newStatus;
    result = reconstruct(end);
    open.clear();
}

std::vector<PathNode> Pathfinder::reconstruct(const BlockPos& end) const {
    std::vector<PathNode> path;
    auto cursor = end;

    while (true) {
        const auto found = nodes.find(cursor);
        if (found == nodes.end())
            return {};
        path.push_back({cursor, found->second.movement, found->second.movementCost});
        if (!found->second.hasParent)
            break;
        cursor = found->second.parent;
    }

    std::reverse(path.begin(), path.end());
    if (!path.empty())
        path.front().movement = MovementType::Start;
    return path;
}

} // namespace baritone
