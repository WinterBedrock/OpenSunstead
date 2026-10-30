#include "BaritoneCore.h"

#include <array>
#include <numbers>

namespace baritone {
namespace {

bool passable(const BlockState& block, const PathOptions& options) {
    return block.loaded && !block.solid && !block.hazard && (!block.liquid || options.allowWater);
}

bool swimmable(const IWorld& world, const BlockPos& pos, const PathOptions& options) {
    if (!options.allowWater)
        return false;
    const auto feet = world.getBlock(pos);
    const auto head = world.getBlock(pos.offset(0, 1, 0));
    return passable(feet, options) && passable(head, options) && feet.liquid && !head.liquid;
}

bool safeWaterLanding(const IWorld& world, const BlockPos& pos) {
    const auto feet = world.getBlock(pos);
    const auto head = world.getBlock(pos.offset(0, 1, 0));
    return feet.loaded && head.loaded && feet.liquid && !feet.hazard &&
        !feet.solid && !head.solid && !head.hazard && !head.liquid;
}

bool hasSafeSupport(const IWorld& world, const BlockPos& pos) {
    const auto support = world.getBlock(pos.offset(0, -1, 0));
    return support.loaded && support.solid && !support.hazard;
}

} // namespace

bool MovementGenerator::canOccupy(const IWorld& world, const BlockPos& pos, const PathOptions& options) {
    return passable(world.getBlock(pos), options) && passable(world.getBlock(pos.offset(0, 1, 0)), options);
}

bool MovementGenerator::canStandAt(const IWorld& world, const BlockPos& pos, const PathOptions& options) {
    if (!canOccupy(world, pos, options))
        return false;
    if (world.getBlock(pos).liquid || world.getBlock(pos.offset(0, 1, 0)).liquid)
        return swimmable(world, pos, options);
    return hasSafeSupport(world, pos) || swimmable(world, pos, options);
}

bool MovementGenerator::canDropToWater(const IWorld& world, const BlockPos& from, const BlockPos& to) {
    if (to.y >= from.y || from.y - to.y > 384 ||
        std::abs(to.x - from.x) + std::abs(to.z - from.z) != 1 || !safeWaterLanding(world, to))
        return false;
    for (int y = to.y + 1; y <= from.y + 1; ++y) {
        const auto state = world.getBlock({to.x, y, to.z});
        if (!state.loaded || state.solid || state.hazard || state.liquid)
            return false;
    }
    return true;
}

void MovementGenerator::getMovements(const IWorld& world, const BlockPos& from,
                                     const PathOptions& options, std::vector<Movement>& result) {
    static constexpr std::array<std::pair<int, int>, 8> directions{{
        {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}
    }};

    result.clear();
    if (result.capacity() < 24)
        result.reserve(24);

    const bool fromWater = options.allowWater && world.getBlock(from).liquid;
    if (fromWater && world.getBlock(from.offset(0, 1, 0)).liquid) {
        const auto up = from.offset(0, 1, 0);
        if (canOccupy(world, from, options) && canOccupy(world, up, options))
            result.push_back({up, MovementType::Swim, action_costs::walkInWater * 1.5});
        return;
    }

    for (const auto [dx, dz] : directions) {
        const bool diagonal = dx != 0 && dz != 0;
        if (diagonal && !options.allowDiagonal)
            continue;

        if (diagonal) {
            if (!canOccupy(world, from.offset(dx, 0, 0), options) ||
                !canOccupy(world, from.offset(0, 0, dz), options))
                continue;
        }

        const auto adjacent = from.offset(dx, 0, dz);
        const double horizontalCost = diagonal ? std::numbers::sqrt2 : 1.0;

        if (canStandAt(world, adjacent, options)) {
            const bool water = fromWater || swimmable(world, adjacent, options);
            const double baseCost = water ? action_costs::walkInWater :
                (options.preferSprint ? action_costs::sprintOneBlock : action_costs::walkOneBlock);
            result.push_back({adjacent, water ? MovementType::Swim :
                (diagonal ? MovementType::Diagonal : MovementType::Traverse), horizontalCost * baseCost});
            continue;
        }

        if (!diagonal && options.allowAscend) {
            const auto up = adjacent.offset(0, 1, 0);
            if (canStandAt(world, up, options) && canOccupy(world, from.offset(0, 1, 0), options)) {
                result.push_back({up, MovementType::Ascend, action_costs::jumpOneBlock});
                continue;
            }
        }

        if (!diagonal && options.allowFall) {
            const auto down = adjacent.offset(0, -1, 0);
            if (canStandAt(world, down, options) && canOccupy(world, adjacent, options)) {
                result.push_back({down, MovementType::Descend,
                    action_costs::walkOffBlock + action_costs::fallTicks(1)});
                continue;
            }
        }

        if (options.allowWater && options.allowFall && !fromWater && !diagonal) {
            const auto descentEntryHead = world.getBlock(adjacent.offset(0, 1, 0));
            if (descentEntryHead.loaded && !descentEntryHead.solid && !descentEntryHead.hazard) {
                constexpr int maximumLoadedDrop = 384;
                for (int drop = 2; drop <= maximumLoadedDrop; ++drop) {
                    const auto cell = adjacent.offset(0, -drop, 0);
                    const auto feetBlock = world.getBlock(cell);
                    const auto headBlock = world.getBlock(cell.offset(0, 1, 0));
                    if (!feetBlock.loaded || !headBlock.loaded || feetBlock.solid ||
                        headBlock.solid || feetBlock.hazard || headBlock.hazard)
                        break;
                    if (safeWaterLanding(world, cell) && canDropToWater(world, from, cell)) {
                        result.push_back({cell, MovementType::WaterDrop,
                            action_costs::walkOffBlock + action_costs::fallTicks(drop) * 0.72 +
                            action_costs::centerAfterFall});
                        break;
                    }
                }
            }
        }

        if (options.allowFall && !diagonal) {
            for (int drop = 2; drop <= options.maxFallHeight; ++drop) {
                const auto landing = adjacent.offset(0, -drop, 0);
                if (!canOccupy(world, landing.offset(0, 1, 0), options))
                    break;
                if (canStandAt(world, landing, options)) {
                    result.push_back({landing, MovementType::Fall,
                        action_costs::walkOffBlock + action_costs::fallTicks(drop) +
                        action_costs::centerAfterFall});
                    break;
                }
            }
        }
    }

    if (options.allowParkour && !fromWater && canStandAt(world, from, options) &&
        canOccupy(world, from.offset(0, 2, 0), options)) {
        static constexpr std::array<std::pair<int, int>, 4> cardinal{{
            {1, 0}, {-1, 0}, {0, 1}, {0, -1}
        }};
        const int maxDistance = std::clamp(options.maxParkourDistance, 2, 4);

        for (const auto [dx, dz] : cardinal) {
            const auto adjacent = from.offset(dx, 0, dz);
            if (canStandAt(world, adjacent, options) || !canOccupy(world, adjacent, options))
                continue;

            bool shallowGap = false;
            for (int drop = 1; drop <= 2; ++drop) {
                if (canStandAt(world, adjacent.offset(0, -drop, 0), options)) {
                    shallowGap = true;
                    break;
                }
            }
            if (shallowGap)
                continue;

            for (int distance = 2; distance <= maxDistance; ++distance) {
                const auto candidate = from.offset(dx * distance, 0, dz * distance);
                bool clearArc = true;
                for (int step = 2; step < distance; ++step) {
                    const auto column = from.offset(dx * step, 0, dz * step);
                    if (!canOccupy(world, column, options)) {
                        clearArc = false;
                        break;
                    }
                }
                if (!clearArc)
                    break;

                if (canStandAt(world, candidate, options) &&
                    canOccupy(world, candidate.offset(0, 2, 0), options)) {
                    result.push_back({candidate, MovementType::Parkour,
                        action_costs::parkourJump(distance, options.preferSprint)});
                    break;
                }

                if (options.allowAscend && distance <= 3) {
                    const auto raised = candidate.offset(0, 1, 0);
                    if (canStandAt(world, raised, options) &&
                        canOccupy(world, candidate.offset(0, 2, 0), options)) {
                        result.push_back({raised, MovementType::Parkour,
                            action_costs::parkourJump(distance, true) + action_costs::jumpOneBlock});
                        break;
                    }
                }
            }
        }
    }
}

} // namespace baritone
