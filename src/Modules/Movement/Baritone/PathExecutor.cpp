#include "PathExecutor.h"
#include "BedrockPhysics.h"
#include "BaritoneWorld.h"

#include "../../../SDK/SDK.h"
#include "../../../SDK/Client/MoveInputComponent.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace baritone {
namespace {

using bedrock_physics::playerHalfWidth;

struct SegmentProjection {
    float progress = 0.f;
    float lateralDistance = 0.f;
};

// Projects the player onto the infinite route line; clamping to the segment
// would turn longitudinal overshoot into "lateral" error and make a straight
// sprint look like it left the path.
SegmentProjection projectOntoSegment(const Vec3& point, const BlockPos& from, const BlockPos& to) {
    const float sx = (float)from.x + 0.5f, sz = (float)from.z + 0.5f;
    const float ex = (float)to.x + 0.5f, ez = (float)to.z + 0.5f;
    const float segX = ex - sx, segZ = ez - sz;
    const float lengthSquared = segX * segX + segZ * segZ;
    if (lengthSquared <= 0.001f)
        return {0.f, std::sqrt((point.x - sx) * (point.x - sx) + (point.z - sz) * (point.z - sz))};
    const float t = ((point.x - sx) * segX + (point.z - sz) * segZ) / lengthSquared;
    const float cx = sx + segX * t, cz = sz + segZ * t;
    return {t, std::sqrt((point.x - cx) * (point.x - cx) + (point.z - cz) * (point.z - cz))};
}

float horizontalDistance(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

bool hasSafeSupport(const BlockSourceWorld& world, const BlockPos& feet) {
    const auto support = world.getBlockNow(feet.offset(0, -1, 0));
    return support.loaded && support.solid && !support.hazard;
}

bool hasPlayerClearance(const BlockSourceWorld& world, const BlockPos& feet, bool allowLiquid) {
    const auto feetBlock = world.getBlockNow(feet);
    const auto headBlock = world.getBlockNow(feet.offset(0, 1, 0));
    return feetBlock.loaded && headBlock.loaded && !feetBlock.solid && !headBlock.solid &&
        !feetBlock.hazard && !headBlock.hazard &&
        (allowLiquid || (!feetBlock.liquid && !headBlock.liquid));
}

bool liquidAt(const BlockSourceWorld& world, const BlockPos& pos) {
    const auto state = world.getBlockNow(pos);
    return state.loaded && state.liquid;
}

} // namespace

unsigned long long PathExecutor::nowMs() {
    return GetTickCount64();
}

void PathExecutor::begin(std::vector<PathNode> newPath) {
    path = std::move(newPath);
    index = path.size() > 1 ? 1 : path.size();
    lastProgressPosition = {};
    lastMotionPosition = {};
    progressInitialized = false;
    motionSampleInitialized = false;
    ticksWithoutProgress = 0;
    ticksOutsidePath = 0;
    lastFailureReason = "";
    travelYawActive = false;
    resetAscendState();
    resetParkourState();
}

void PathExecutor::stop(SDK::Actor* player) {
    clearInput(player);
    path.clear();
    index = 0;
    ticksWithoutProgress = 0;
    ticksOutsidePath = 0;
    progressInitialized = false;
    motionSampleInitialized = false;
    travelYawActive = false;
    lastFailureReason = "";
    resetAscendState();
    resetParkourState();
}

void PathExecutor::resetAscendState() {
    activeAscendIndex = static_cast<std::size_t>(-1);
    ascendJumpIssued = false;
    ascendWasAirborne = false;
    ascendLaunchTicks = 0;
    ascendRetries = 0;
    ascendRetryDelay = 0;
}

void PathExecutor::resetParkourState() {
    activeParkourIndex = static_cast<std::size_t>(-1);
    parkourJumpIssued = false;
    parkourWasAirborne = false;
    parkourRepositioning = false;
    parkourRunwayPrepared = false;
    parkourAirControlActive = false;
    parkourSprintTicks = 0;
    parkourLaunchTicks = 0;
}

void PathExecutor::clearInput(SDK::Actor* player) {
    travelYawActive = false;
    if (!player)
        return;
    auto* input = SDK::getMoveInput(player);
    if (!input)
        return;
    __try {
        input->inputState = {};
        input->rawInputState = {};
        input->move = {};
        input->tail &= ~(SDK::TailSprinting | SDK::TailJumping | SDK::TailMoveLocked);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void PathExecutor::writeInput(SDK::Actor* player, const float forwardAmount,
                              const float strafeAmount, const bool sprint,
                              const bool jump, const bool sneak) {
    auto* input = SDK::getMoveInput(player);
    if (!input)
        return;

    // Sanitize into vanilla's stick bounds: finite, inside the unit circle,
    // deadzone-cleared. Bedrock's own pipeline enforces these before the
    // movement prediction and AuthInput see the vector.
    Vec2 movement(strafeAmount, forwardAmount);
    const float magnitudeSquared = movement.x * movement.x + movement.y * movement.y;
    if (!std::isfinite(movement.x) || !std::isfinite(movement.y)) {
        movement = {};
    } else if (magnitudeSquared > 1.f) {
        const float magnitude = std::sqrt(magnitudeSquared);
        movement.x /= magnitude;
        movement.y /= magnitude;
    }
    if (std::abs(movement.x) < 0.02f) movement.x = 0.f;
    if (std::abs(movement.y) < 0.02f) movement.y = 0.f;

    __try {
        const bool jumpWasDown = input->rawInputState.jumpInputCurrentlyDown;
        const bool sneakWasDown = input->rawInputState.sneakInputCurrentlyDown;

        input->inputState.analogMoveVector = movement;
        input->rawInputState.analogMoveVector = movement;
        input->move = movement;

        input->inputState.up = input->rawInputState.up = movement.y > 0.f;
        input->inputState.down = input->rawInputState.down = movement.y < 0.f;
        input->inputState.left = input->rawInputState.left = movement.x < 0.f;
        input->inputState.right = input->rawInputState.right = movement.x > 0.f;
        input->inputState.upLeft = input->rawInputState.upLeft =
            input->inputState.up && input->inputState.left;
        input->inputState.upRight = input->rawInputState.upRight =
            input->inputState.up && input->inputState.right;
        input->inputState.downLeft = input->rawInputState.downLeft =
            input->inputState.down && input->inputState.left;
        input->inputState.downRight = input->rawInputState.downRight =
            input->inputState.down && input->inputState.right;

        input->inputState.sprintDown = input->rawInputState.sprintDown = sprint;
        if (sprint)
            input->tail |= SDK::TailSprinting;
        else
            input->tail &= ~SDK::TailSprinting;

        input->inputState.jumpDown = input->rawInputState.jumpDown = jump;
        input->inputState.jumpInputCurrentlyDown = jump;
        input->rawInputState.jumpInputCurrentlyDown = jump;
        input->inputState.jumpInputWasPressed = input->rawInputState.jumpInputWasPressed =
            jump && !jumpWasDown;
        input->inputState.jumpInputWasReleased = input->rawInputState.jumpInputWasReleased =
            !jump && jumpWasDown;
        if (jump)
            input->tail |= SDK::TailJumping;
        else
            input->tail &= ~SDK::TailJumping;

        input->inputState.sneakDown = input->rawInputState.sneakDown = sneak;
        input->inputState.sneakInputCurrentlyDown = sneak;
        input->rawInputState.sneakInputCurrentlyDown = sneak;
        input->inputState.sneakInputWasPressed = input->rawInputState.sneakInputWasPressed =
            sneak && !sneakWasDown;
        input->inputState.sneakInputWasReleased = input->rawInputState.sneakInputWasReleased =
            !sneak && sneakWasDown;

        // Never lock the component: the game's own input tick stays the
        // pipeline owner and keeps the packet-side state coherent.
        input->tail &= ~SDK::TailMoveLocked;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

ExecutionStatus PathExecutor::tick(SDK::Actor* player, const BlockSourceWorld& world,
                                  const ExecutionOptions& options) {
    if (!player)
        return ExecutionStatus::NoPlayer;

    if (index >= path.size()) {
        clearInput(player);
        return ExecutionStatus::Arrived;
    }

    auto* moveInput = SDK::getMoveInput(player);
    if (!moveInput) {
        lastFailureReason = "vanilla movement input unavailable";
        return ExecutionStatus::Stuck;
    }

    auto* state = player->getStateVector();
    if (!state) {
        lastFailureReason = "state vector unavailable";
        return ExecutionStatus::Stuck;
    }
    auto* aabb = player->getAabbs();
    if (!aabb) {
        lastFailureReason = "aabb unavailable";
        return ExecutionStatus::Stuck;
    }

    const Vec3 feet = state->pos;
    // posPrev is already synchronized to pos by the time this callback runs,
    // so per-tick displacement is measured ourselves.
    Vec3 measuredMotion{};
    if (motionSampleInitialized)
        measuredMotion = feet - lastMotionPosition;
    lastMotionPosition = feet;
    motionSampleInitialized = true;
    if (!std::isfinite(measuredMotion.x) || !std::isfinite(measuredMotion.y) ||
        !std::isfinite(measuredMotion.z) ||
        std::sqrt(measuredMotion.x * measuredMotion.x + measuredMotion.z * measuredMotion.z) > 1.25f)
        measuredMotion = {};

    const bool onGround = player->isOnGround();
    const BlockPos playerFeetBlock((int)std::floor(feet.x), (int)std::floor(feet.y + 0.1251f),
                                   (int)std::floor(feet.z));
    const bool inWaterBlocks = liquidAt(world, playerFeetBlock) ||
        liquidAt(world, playerFeetBlock.offset(0, 1, 0));

    if (!progressInitialized) {
        lastProgressPosition = feet;
        progressInitialized = true;
    }

    // ---- consume reached nodes ------------------------------------------
    // Baritone's success rule: the physical feet block must BE the movement
    // destination, with relaxed rules for falls and swims where Bedrock
    // momentum routinely carries past block centres.
    bool outsideActiveCorridor = false;
    if (onGround && index > 0 && index < path.size()) {
        const auto projection = projectOntoSegment(feet, path[index - 1].pos, path[index].pos);
        const float corridorWidth = path[index].movement == MovementType::Swim ? 1.05f : 0.82f;
        outsideActiveCorridor = projection.lateralDistance > corridorWidth;
    }

    while (!outsideActiveCorridor && index < path.size()) {
        const auto& node = path[index];
        bool reached = playerFeetBlock == node.pos &&
            (onGround || node.movement == MovementType::Swim);

        if (!reached && node.movement == MovementType::WaterDrop && inWaterBlocks) {
            const float dx = feet.x - ((float)node.pos.x + 0.5f);
            const float dz = feet.z - ((float)node.pos.z + 0.5f);
            reached = dx * dx + dz * dz <= 0.9f && feet.y <= (float)node.pos.y + 1.5f;
        }
        if (!reached && node.movement == MovementType::Swim) {
            const float dx = feet.x - ((float)node.pos.x + 0.5f);
            const float dz = feet.z - ((float)node.pos.z + 0.5f);
            // Surface buoyancy floats the feet above the route cell.
            reached = dx * dx + dz * dz <= 0.62f * 0.62f &&
                feet.y >= (float)node.pos.y - 0.15f && feet.y <= (float)node.pos.y + 1.05f;
        }

        // Falls land with forward momentum; accept the supported landing
        // corridor instead of steering back toward the top of the drop.
        if (!reached && index > 0 && onGround &&
            (node.movement == MovementType::Descend || node.movement == MovementType::Fall) &&
            std::abs(feet.y - (float)node.pos.y) <= 0.85f) {
            const auto landing = projectOntoSegment(feet, path[index - 1].pos, node.pos);
            reached = hasSafeSupport(world, playerFeetBlock) &&
                landing.progress >= 0.f && landing.progress <= 1.45f &&
                landing.lateralDistance <= 0.5f;
        }

        // Jump overclimb: a parkour or ascend landing one node past its
        // target is real progress, not a failure.
        if (!reached && onGround &&
            ((activeParkourIndex == index && parkourJumpIssued && parkourWasAirborne) ||
             (activeAscendIndex == index && ascendJumpIssued && ascendWasAirborne))) {
            const auto& source = path[index - 1].pos;
            const int stepX = std::clamp(node.pos.x - source.x, -1, 1);
            const int stepZ = std::clamp(node.pos.z - source.z, -1, 1);
            const BlockPos overshoot = node.pos.offset(stepX, 0, stepZ);
            if (playerFeetBlock == overshoot && hasSafeSupport(world, overshoot)) {
                index += 1;
                lastProgressPosition = feet;
                ticksWithoutProgress = 0;
                ticksOutsidePath = 0;
                resetParkourState();
                resetAscendState();
                continue;
            }
        }

        if (!reached)
            break;
        ++index;
    }

    if (index >= path.size()) {
        clearInput(player);
        return ExecutionStatus::Arrived;
    }

    // ---- off-path guard --------------------------------------------------
    if (onGround && index > 0) {
        const auto missed = projectOntoSegment(feet, path[index - 1].pos, path[index].pos);
        const bool drop = path[index].movement == MovementType::Descend ||
            path[index].movement == MovementType::Fall;
        if (missed.progress > (drop ? 2.25f : 1.35f)) {
            clearInput(player);
            lastFailureReason = "overshot movement node";
            return ExecutionStatus::OffPath;
        }
    }
    if (outsideActiveCorridor) {
        if (++ticksOutsidePath > 4) {
            clearInput(player);
            lastFailureReason = "outside movement corridor";
            return ExecutionStatus::OffPath;
        }
    } else {
        ticksOutsidePath = 0;
    }

    // ---- progress / stall detection --------------------------------------
    const bool waterRoute = path[index].movement == MovementType::Swim || inWaterBlocks;
    if (!onGround && !inWaterBlocks && !waterRoute) {
        lastProgressPosition = feet;
        ticksWithoutProgress = 0;
    } else if (horizontalDistance(feet, lastProgressPosition) > 0.15f ||
        (!inWaterBlocks && !waterRoute && std::abs(feet.y - lastProgressPosition.y) > 0.35f)) {
        lastProgressPosition = feet;
        ticksWithoutProgress = 0;
    } else if (++ticksWithoutProgress > 80) {
        clearInput(player);
        lastFailureReason = "no movement progress";
        return ExecutionStatus::Stuck;
    }

    const auto& node = path[index];

    // ---- per-movement steering -------------------------------------------
    const Vec3 target((float)node.pos.x + 0.5f, (float)node.pos.y, (float)node.pos.z + 0.5f);
    Vec2 direction(target.x - feet.x, target.z - feet.z);
    Vec2 facingDirection = direction;

    const bool isAscending = node.movement == MovementType::Ascend;
    const bool isParkour = node.movement == MovementType::Parkour;
    const bool isBridgeLikeDrop = node.movement == MovementType::Descend ||
        node.movement == MovementType::Fall || node.movement == MovementType::WaterDrop;
    const bool ordinaryMovement = node.movement == MovementType::Traverse ||
        node.movement == MovementType::Diagonal;

    bool ascendTakeoffReady = !isAscending;
    float ascendApproachScale = 1.f;
    if (isAscending && index > 0) {
        const auto& source = path[index - 1].pos;
        if (activeAscendIndex != index) {
            resetAscendState();
            activeAscendIndex = index;
        } else {
            if (ascendRetryDelay > 0)
                --ascendRetryDelay;
            if (ascendJumpIssued) {
                ++ascendLaunchTicks;
                if (!onGround)
                    ascendWasAirborne = true;
                if (bedrock_physics::shouldRetryAscent(onGround, feet.y, (float)node.pos.y,
                                                       ascendWasAirborne, ascendLaunchTicks)) {
                    if (++ascendRetries > 3) {
                        clearInput(player);
                        lastFailureReason = "ascent retries exhausted";
                        return ExecutionStatus::Stuck;
                    }
                    ascendJumpIssued = false;
                    ascendWasAirborne = false;
                    ascendLaunchTicks = 0;
                    ascendRetryDelay = 3;
                    ticksWithoutProgress = 0;
                }
            }
        }

        Vec2 axis((float)(node.pos.x - source.x), (float)(node.pos.z - source.z));
        const float axisLength = std::sqrt(axis.x * axis.x + axis.y * axis.y);
        if (axisLength > 0.001f)
            axis /= axisLength;
        const Vec2 sourceCenter((float)source.x + 0.5f, (float)source.z + 0.5f);
        const Vec2 sourceOffset(feet.x - sourceCenter.x, feet.z - sourceCenter.y);
        const float along = sourceOffset.x * axis.x + sourceOffset.y * axis.y;
        const float lateral = std::sqrt(std::pow(sourceOffset.x - axis.x * along, 2.f) +
            std::pow(sourceOffset.y - axis.y * along, 2.f));
        const float alongSpeed = measuredMotion.x * axis.x + measuredMotion.z * axis.y;
        // Launch from the forward half of the source block, centred on the
        // axis: jumping off the rear edge clips the source ceiling.
        ascendTakeoffReady = onGround && playerFeetBlock == source &&
            lateral <= 0.24f && along >= 0.04f &&
            along + std::max(0.f, alongSpeed) >= 0.12f && along <= 0.58f &&
            ascendRetryDelay == 0;
        if (!inWaterBlocks && !ascendJumpIssued && !ascendTakeoffReady) {
            const Vec2 takeoffPoint = sourceCenter + axis * 0.18f;
            direction = takeoffPoint - Vec2(feet.x, feet.z);
            facingDirection = axis;
            ascendApproachScale = 0.62f;
        }
    } else if (!isAscending) {
        resetAscendState();
    }
    if (isAscending && inWaterBlocks)
        ascendJumpIssued = true; // swim into the bank; never retreat

    // Parkour steering: runway, sprint gate, takeoff window, air control.
    int parkourDistance = 0;
    bool parkourAscend = false;
    bool parkourReady = !isParkour;
    float parkourAlong = 0.f;
    float parkourProgress = 0.f;
    float parkourAlongSpeed = 0.f;
    float parkourLateralCorrection = 0.f;
    if (isParkour && index > 0) {
        const auto& source = path[index - 1].pos;
        const Vec2 sourceCenter((float)source.x + 0.5f, (float)source.z + 0.5f);
        Vec2 jumpDirection((float)(node.pos.x - source.x), (float)(node.pos.z - source.z));
        parkourDistance = std::abs(node.pos.x - source.x) + std::abs(node.pos.z - source.z);
        parkourAscend = node.pos.y > source.y;

        const float jumpLength = std::sqrt(jumpDirection.x * jumpDirection.x + jumpDirection.y * jumpDirection.y);
        if (jumpLength > 0.001f) {
            jumpDirection /= jumpLength;
            facingDirection = jumpDirection;
            const Vec2 fromSource(feet.x - sourceCenter.x, feet.z - sourceCenter.y);
            parkourAlong = fromSource.x * jumpDirection.x + fromSource.y * jumpDirection.y;
            parkourProgress = parkourAlong / jumpLength;
            const Vec2 lateralOffset(fromSource.x - jumpDirection.x * parkourAlong,
                                     fromSource.y - jumpDirection.y * parkourAlong);
            const float lateral = std::sqrt(lateralOffset.x * lateralOffset.x + lateralOffset.y * lateralOffset.y);
            parkourAlongSpeed = measuredMotion.x * jumpDirection.x + measuredMotion.z * jumpDirection.y;
            parkourReady = onGround && lateral <= 0.45f &&
                parkourAlong >= -0.65f && parkourAlong <= bedrock_physics::safeTakeoffEdge;
            const bool needsRunway = parkourDistance >= 3 || parkourAscend;
            const bool speedReady = bedrock_physics::parkourSprintReady(
                parkourDistance, parkourAscend, options.sprint, parkourSprintTicks, parkourAlongSpeed);
            if (!parkourJumpIssued && onGround && needsRunway && !speedReady && parkourAlong >= 0.25f)
                parkourRepositioning = true;
            if (parkourRepositioning) {
                const Vec2 runwayStart = sourceCenter - jumpDirection * 0.20f;
                const Vec2 runwayError = runwayStart - Vec2(feet.x, feet.z);
                const Vec2 horizontalMotion(measuredMotion.x, measuredMotion.z);
                direction = runwayError - horizontalMotion * 2.25f;
                if (std::sqrt(runwayError.x * runwayError.x + runwayError.y * runwayError.y) <= 0.14f &&
                    lateral <= 0.20f && std::abs(parkourAlongSpeed) < 0.065f) {
                    parkourRepositioning = false;
                    if (parkourDistance == 3)
                        parkourRunwayPrepared = true;
                    parkourSprintTicks = 0;
                }
                parkourReady = false;
            }
            if (!parkourRepositioning && (parkourReady || parkourJumpIssued)) {
                direction = jumpDirection - lateralOffset * (onGround ? 0.65f : 1.10f);
                parkourLateralCorrection = lateral;
            } else if (!parkourRepositioning) {
                direction = sourceCenter - Vec2(feet.x, feet.z);
            }
        }
    } else if (!isParkour) {
        resetParkourState();
    }

    // Falls keep the route tangent and pull toward the landing centreline
    // with air control instead of aiming at the destination centre.
    bool fallControlActive = false;
    bool cautiousDropSneak = false;
    Vec2 fallWorldInput{};
    if (isBridgeLikeDrop && index > 0 &&
        (!onGround || playerFeetBlock == path[index - 1].pos)) {
        const auto& source = path[index - 1].pos;
        Vec2 fallVector((float)(node.pos.x - source.x), (float)(node.pos.z - source.z));
        const float fallLength = std::sqrt(fallVector.x * fallVector.x + fallVector.y * fallVector.y);
        if (fallLength > 0.001f) {
            const int verticalDrop = std::max(0, source.y - node.pos.y);
            const bool longDrop = verticalDrop >= 2 && node.movement != MovementType::WaterDrop;
            const Vec2 fallDirection = fallVector / fallLength;
            const Vec2 sourceCenter((float)source.x + 0.5f, (float)source.z + 0.5f);
            const Vec2 fromSource(feet.x - sourceCenter.x, feet.z - sourceCenter.y);
            const float along = fromSource.x * fallDirection.x + fromSource.y * fallDirection.y;
            const Vec2 lateralOffset(fromSource.x - fallDirection.x * along,
                                     fromSource.y - fallDirection.y * along);
            direction = fallDirection - lateralOffset * 1.35f;
            facingDirection = fallDirection;

            if (node.movement == MovementType::WaterDrop && !inWaterBlocks) {
                const Vec2 correction(
                    bedrock_physics::waterDropAxisInput(target.x - feet.x, measuredMotion.x, onGround),
                    bedrock_physics::waterDropAxisInput(target.z - feet.z, measuredMotion.z, onGround));
                const float magnitude = std::sqrt(correction.x * correction.x + correction.y * correction.y);
                const Vec2 clamped = magnitude > 1.f ? correction / magnitude : correction;
                direction = clamped;
            } else if (node.movement != MovementType::WaterDrop) {
                const auto projection = projectOntoSegment(feet, source, node.pos);
                const float alongSpeed = measuredMotion.x * fallDirection.x + measuredMotion.z * fallDirection.y;
                const int landingTicks = bedrock_physics::ticksUntilLandingPlane(
                    feet.y, measuredMotion.y, target.y, 30);
                const bool cautiousDrop = longDrop && (!onGround ||
                    (projection.progress + bedrock_physics::projectedAirDisplacement(
                        std::max(alongSpeed, 0.f), 1.f, false, false, landingTicks) / fallLength) > 1.15f);
                const float alongInput = onGround
                    ? (cautiousDrop ? (alongSpeed <= 0.060f ? 1.f : 0.f)
                                   : std::max(0.f, bedrock_physics::groundInputForTargetVelocity(alongSpeed, 0.10f)))
                    : bedrock_physics::fallLandingInput(projection.progress, alongSpeed,
                                                       fallLength, landingTicks,
                                                       cautiousDrop ? 0.80f : 0.90f);
                fallWorldInput = cautiousDrop && !onGround
                    ? Vec2{}
                    : fallDirection * alongInput - lateralOffset * 1.35f;
                const float fallMagnitude = std::sqrt(fallWorldInput.x * fallWorldInput.x +
                    fallWorldInput.y * fallWorldInput.y);
                if (fallMagnitude > 1.f)
                    fallWorldInput /= fallMagnitude;
                cautiousDropSneak = cautiousDrop && onGround && projection.progress < 0.78f;
                fallControlActive = true;
            }
        }
    }

    float directionLength = std::sqrt(direction.x * direction.x + direction.y * direction.y);
    if (directionLength > 0.001f)
        direction /= directionLength;

    // Ease into waypoints like a player instead of holding a unit input to the
    // exact block boundary; parkour and falls keep their physics profiles.
    const float humanApproachScale = ordinaryMovement
        ? std::clamp(directionLength / 1.15f, 0.82f, 1.f)
        : 1.f;

    float facingDistance = std::sqrt(facingDirection.x * facingDirection.x + facingDirection.y * facingDirection.y);
    if (facingDistance > 0.001f)
        facingDirection /= facingDistance;

    // Path facing is a render/packet target only; real camera yaw stays
    // authoritative and the input below is expressed in camera-relative terms.
    if (facingDistance > 0.001f && options.faceTravel) {
        travelYaw = std::atan2(-facingDirection.x, facingDirection.y) * 180.f /
            std::numbers::pi_v<float>;
        travelYawActive = true;
    } else {
        travelYawActive = false;
    }

    const float yaw = player->getRotation() ? player->getRotation()->rotation.y * std::numbers::pi_v<float> / 180.f : 0.f;
    const Vec2 forward(-std::sin(yaw), std::cos(yaw));
    const Vec2 right(forward.y, -forward.x);

    const bool parkourNeedsSprint = isParkour && (parkourDistance >= 3 || parkourAscend);
    const float requestedForward = std::clamp(forward.x * direction.x + forward.y * direction.y, -1.f, 1.f);
    const float pathMovementScale = (parkourRepositioning ? 0.35f : 1.f) *
        humanApproachScale * ascendApproachScale;

    // Decide the takeoff before crossing the launch point: at full sprint the
    // player covers ~0.28 blocks per tick, so a fixed trigger walks the
    // 0.6-wide body off the source block first.
    const float measuredApproachSpeed = std::max(parkourAlongSpeed, 0.f);
    const float nextApproachSpeed = bedrock_physics::nextGroundVelocity(measuredApproachSpeed, 1.f, true, false);
    const float nextParkourAlong = parkourAlong + nextApproachSpeed;
    const float baseTakeoffDistance = parkourAscend ? 0.34f :
        (parkourDistance >= 4 ? 0.42f : (parkourDistance == 3 ? 0.62f : 0.22f));
    const float takeoffDistance = baseTakeoffDistance -
        std::clamp((measuredApproachSpeed - 0.18f) * 0.75f, 0.f, 0.075f);
    const bool sprintReady = bedrock_physics::parkourSprintReady(
        parkourDistance, parkourAscend, options.sprint, parkourSprintTicks, measuredApproachSpeed);
    const bool requestParkourJump = isParkour && parkourReady && !parkourJumpIssued && sprintReady &&
        nextParkourAlong >= takeoffDistance;
    const bool parkourApproachRelease = isParkour && parkourReady && !parkourJumpIssued &&
        !requestParkourJump && nextParkourAlong >= bedrock_physics::safeTakeoffEdge - 0.08f;

    float predictedLandingWithForward = parkourAlong;
    float predictedLandingCoasting = parkourAlong;
    if (isParkour && parkourJumpIssued && !onGround) {
        const float jumpLength = (float)std::max(parkourDistance, 1);
        const int landingTicks = bedrock_physics::ticksUntilLandingPlane(feet.y, measuredMotion.y, target.y, 30);
        predictedLandingWithForward += bedrock_physics::projectedAirDisplacement(
            measuredApproachSpeed, 1.f, true, false, landingTicks);
        predictedLandingCoasting += bedrock_physics::projectedAirDisplacement(
            measuredApproachSpeed, 0.f, false, false, landingTicks);
        const bool releaseCandidate = bedrock_physics::shouldBeginParkourAirControl(
            parkourDistance, parkourProgress, jumpLength,
            predictedLandingWithForward, predictedLandingCoasting);
        if (parkourDistance == 3 && releaseCandidate)
            parkourAirControlActive = true;
    }
    const bool parkourAirRelease = isParkour && parkourJumpIssued && !onGround &&
        (parkourDistance == 3 ? parkourAirControlActive
                              : bedrock_physics::shouldBeginParkourAirControl(
                                    parkourDistance, parkourProgress, (float)std::max(parkourDistance, 1),
                                    predictedLandingWithForward, predictedLandingCoasting));
    const bool parkourAirBrake = parkourAirRelease &&
        predictedLandingCoasting >= (float)parkourDistance + (parkourDistance == 3 ? 0.15f : 0.25f);

    float forwardAmount = parkourAirBrake ? (parkourDistance == 3 ? -1.0f : -0.24f) :
        (parkourAirRelease ? 0.f : (parkourApproachRelease ? 0.f : requestedForward * pathMovementScale));
    float strafeAmount = std::clamp(right.x * direction.x + right.y * direction.y, -1.f, 1.f);
    if (parkourAirRelease || parkourAirBrake)
        strafeAmount *= std::clamp(parkourLateralCorrection * 2.5f, 0.25f, 1.f);

    if (fallControlActive) {
        forwardAmount = forward.x * fallWorldInput.x + forward.y * fallWorldInput.y;
        strafeAmount = right.x * fallWorldInput.x + right.y * fallWorldInput.y;
    }

    const bool shouldSprint = options.sprint && !parkourRepositioning &&
        forwardAmount > 0.8f && (!isParkour || parkourNeedsSprint) && !parkourAirRelease &&
        !inWaterBlocks && !cautiousDropSneak;

    const bool shouldJump = (isAscending && !ascendJumpIssued && ascendTakeoffReady &&
        target.y > feet.y + 0.2f) || requestParkourJump ||
        (inWaterBlocks && (node.movement == MovementType::Swim || target.y >= feet.y - 0.15f));

    // One-block obstacle on a supposedly level path: slabs, fence posts and
    // farmland edges sit between whole-block classifications. Jump when the
    // cell ahead has collision and the two above it do not.
    bool obstacleJump = false;
    if (!shouldJump && onGround && ordinaryMovement) {
        const int aheadX = (int)std::floor(feet.x + direction.x * 0.35f);
        const int aheadZ = (int)std::floor(feet.z + direction.y * 0.35f);
        const auto feetAhead = world.getBlockNow({aheadX, playerFeetBlock.y, aheadZ});
        const auto headAhead = world.getBlockNow({aheadX, playerFeetBlock.y + 1, aheadZ});
        const auto aboveAhead = world.getBlockNow({aheadX, playerFeetBlock.y + 2, aheadZ});
        obstacleJump = feetAhead.loaded && feetAhead.solid &&
            headAhead.loaded && !headAhead.solid && !headAhead.hazard &&
            aboveAhead.loaded && !aboveAhead.solid && !aboveAhead.hazard;
    }

    const bool groundedAtLaunch = onGround;
    const bool pendingAscendLaunch = isAscending && bedrock_physics::holdJumpUntilAirborne(
        ascendJumpIssued, ascendWasAirborne, groundedAtLaunch, ascendLaunchTicks);
    const bool pendingParkourLaunch = isParkour && bedrock_physics::holdJumpUntilAirborne(
        parkourJumpIssued, parkourWasAirborne, groundedAtLaunch, parkourLaunchTicks);
    const bool holdJump = shouldJump || obstacleJump || hopActive() ||
        pendingAscendLaunch || pendingParkourLaunch;

    if (holdJump && isAscending && groundedAtLaunch) {
        ascendJumpIssued = true;
        ascendWasAirborne = false;
        ascendLaunchTicks = 0;
    }
    if (requestParkourJump) {
        parkourJumpIssued = true;
        parkourLaunchTicks = 0;
    }
    if (isParkour && !onGround)
        parkourWasAirborne = true;
    if (isParkour && parkourJumpIssued && parkourWasAirborne && onGround) {
        // Still on this edge after being airborne: the destination was missed.
        clearInput(player);
        lastFailureReason = "jump landed outside target";
        return ExecutionStatus::Stuck;
    }

    const bool shouldSneak = cautiousDropSneak || (parkourRepositioning && parkourDistance != 3);

    writeInput(player, forwardAmount, strafeAmount, shouldSprint, holdJump, shouldSneak);

    if (isParkour && parkourReady && parkourNeedsSprint && shouldSprint)
        ++parkourSprintTicks;
    else if (!parkourJumpIssued)
        parkourSprintTicks = 0;

    return ExecutionStatus::Running;
}

} // namespace baritone
