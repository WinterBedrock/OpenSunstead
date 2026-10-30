#include "ChestAura.h"

#include "../../Core/Logger.h"
#include "../../SDK/SDK.h"
#include "../../SDK/Network/PacketUtils.h"
#include "../Combat/SilentRotation.h"
#include "../ModuleManager.h"
#include "../Setting.h"
#include "../Visual/Notifications.h"
#include "ChestStealer.h"

#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace GM = SDK::GameMode;
using SDK::sehGetPos;

namespace {

// A refused interaction must not wedge the module: if no container screen shows
// up this soon after firing, give up on that target and move on.
constexpr uint64_t kAwaitScreenTimeoutMs = 900;

// Faces to try, in the order a player would reach one. `face` is the Bedrock
// face id of the container side we click; the vector is the offset from the
// container to the neighbour block on that side (the side is reachable only
// when that neighbour is non-solid). The server validates the transaction
// against BOTH the click face and the HitResult, so the face id MUST match its
// offset or the open is silently refused — which is the exact bug that made the
// module do nothing. Bedrock face ids: 0=down(-Y) 1=up(+Y) 2=north(-Z)
// 3=south(+Z) 4=west(-X) 5=east(+X) (matches Scaffold::sehGetBlockPlaceFace).
struct FaceProbe {
    int face;
    int dx, dy, dz;
};
constexpr FaceProbe kFaces[] = {
    { 1,  0,  1,  0 }, // up    (+Y)
    { 2,  0,  0, -1 }, // north (-Z)
    { 3,  0,  0,  1 }, // south (+Z)
    { 4, -1,  0,  0 }, // west  (-X)
    { 5,  1,  0,  0 }, // east  (+X)
    { 0,  0, -1,  0 }, // down  (-Y)
};

// Point on the clicked face's surface (block-local, 0..1 per axis) so the
// server's HitResult check agrees with the face id we send. Using the block
// centre for every face lets the server reject clicks whose hit point does not
// lie on the named face.
inline void faceHitPoint(int face, float bx, float by, float bz,
                         float* hx, float* hy, float* hz) {
    *hx = bx + 0.5f; *hy = by + 0.5f; *hz = bz + 0.5f;
    switch (face) {
    case 0: *hy = by;        break; // down  (-Y)
    case 1: *hy = by + 1.0f; break; // up    (+Y)
    case 2: *hz = bz;        break; // north (-Z)
    case 3: *hz = bz + 1.0f; break; // south (+Z)
    case 4: *hx = bx;        break; // west  (-X)
    case 5: *hx = bx + 1.0f; break; // east  (+X)
    default: break;
    }
}

std::string lowerNoNamespace(const std::string& raw) {
    std::string n = raw;
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const size_t colon = n.find(':');
    if (colon != std::string::npos) n = n.substr(colon + 1);
    return n;
}

// Shortest distance from a point to the block's 1x1x1 volume — the same metric
// OreMiner uses, so the Range setting reads like vanilla reach rather than
// centre-to-centre distance.
float distanceToBlock(const Vec3& from, const SDK::BlockPos& pos) {
    const Vec3 lo(static_cast<float>(pos.x), static_cast<float>(pos.y), static_cast<float>(pos.z));
    const Vec3 hi = lo + Vec3(1.f, 1.f, 1.f);
    const float cx = std::clamp(from.x, lo.x, hi.x);
    const float cy = std::clamp(from.y, lo.y, hi.y);
    const float cz = std::clamp(from.z, lo.z, hi.z);
    const float dx = cx - from.x, dy = cy - from.y, dz = cz - from.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// OnUpdate is frame-driven, so throttle hard.
SUNSTEAD_DEFINE_DBG(auraDbg, "ChestAura", 300)

// Empty hotbar slot, or -1 when the whole bar is full.
int findEmptyHotbarSlot(SDK::Actor* lp) {
    SDK::Container* c = SDK::Inv::container(lp);
    if (!c) return -1;

    for (int slot = 0; slot < 9; ++slot) {
        SDK::ItemStack* stack = c->getItem(slot);
        if (!stack || !stack->isValidStack()) return slot;
    }
    return -1;
}

} // namespace

ChestAura::ChestAura()
    : Module("ChestAura", "Opens nearby containers automatically", Category::Player, 0) {
    mRange       = add<NumberSetting>("Range", "Maximum reach to a container", 4.5f, 1.f, 7.f, 0.1f);
    mDelay       = add<NumberSetting>("Delay", "Milliseconds between open attempts", 350.f, 50.f, 2000.f, 25.f);

    mSkipVisited = add<BoolSetting>("Skip Opened", "Do not reopen containers already opened this session", true);
    mForgetAfter = add<NumberSetting>("Forget After", "Clear the opened list after this many seconds (0 = never)", 0.f, 0.f, 600.f, 10.f);

    mChests      = add<BoolSetting>("Chests", "Open chests", true);
    mTrapped     = add<BoolSetting>("Trapped Chests", "Open trapped chests", true);
    mBarrels     = add<BoolSetting>("Barrels", "Open barrels", true);
    mShulkers    = add<BoolSetting>("Shulker Boxes", "Open placed shulker boxes", true);
    mEnderChests = add<BoolSetting>("Ender Chests", "Open ender chests", false);

    mRotate      = add<BoolSetting>("Rotate", "Silent-rotate toward the container while opening", true);
    mEmptyHand   = add<BoolSetting>("Empty Hand", "Switch to an empty hotbar slot so a held block is not placed", true);

    mNotify      = add<BoolSetting>("Notify", "Show a notification for each container opened", false);
    mDebug       = add<BoolSetting>("Debug", "Log target selection to logs.txt", false);

    canBind = true;
    UpdateSettingsVisibility();
}

void ChestAura::UpdateSettingsVisibility() {
    mForgetAfter->mVisible = mSkipVisited->mValue;
}

float ChestAura::rangeBlocks() const { return mRange->mValue; }

bool ChestAura::wantsKind(Kind kind) const {
    switch (kind) {
    case Kind::Chest:        return mChests->mValue;
    case Kind::TrappedChest: return mTrapped->mValue;
    case Kind::EnderChest:   return mEnderChests->mValue;
    case Kind::Barrel:       return mBarrels->mValue;
    case Kind::ShulkerBox:   return mShulkers->mValue;
    case Kind::None:         break;
    }
    return false;
}

ChestAura::Kind ChestAura::classify(const std::string& blockName, uint16_t legacyId) const {
    if (!blockName.empty()) {
        const std::string n = lowerNoNamespace(blockName);
        // Order matters: "chest" is a substring of both other chest variants.
        if (n.find("ender_chest") != std::string::npos)   return Kind::EnderChest;
        if (n.find("trapped_chest") != std::string::npos) return Kind::TrappedChest;
        if (n.find("shulker_box") != std::string::npos)   return Kind::ShulkerBox;
        if (n.find("barrel") != std::string::npos)        return Kind::Barrel;
        if (n.find("chest") != std::string::npos)         return Kind::Chest;
        return Kind::None;
    }

    // Name lookup failed (unreadable BlockLegacy) — fall back to legacy ids.
    switch (legacyId) {
    case 54:  return Kind::Chest;
    case 130: return Kind::EnderChest;
    case 146: return Kind::TrappedChest;
    case 218: return Kind::ShulkerBox;
    case 458: return Kind::Barrel;
    default:  return Kind::None;
    }
}

bool ChestAura::findTarget(SDK::ClientInstance* ci, SDK::Actor* lp, Candidate* out) const {
    if (!ci || !lp || !out) return false;

    SDK::BlockSource* src = ci->getBlockSource();
    if (!src) return false;

    Vec3 eye{};
    if (!sehGetPos(lp, &eye)) return false;

    const float range = rangeBlocks();
    const int span = static_cast<int>(std::ceil(range));
    const SDK::BlockPos origin(eye);

    Candidate best;
    best.distance = range + 1.f;

    for (int dx = -span; dx <= span; ++dx) {
        for (int dy = -span; dy <= span; ++dy) {
            for (int dz = -span; dz <= span; ++dz) {
                const SDK::BlockPos pos(origin.x + dx, origin.y + dy, origin.z + dz);

                const float dist = distanceToBlock(eye, pos);
                if (dist > range || dist >= best.distance) continue;
                if (mSkipVisited->mValue && mVisited.count(pos)) continue;

                SDK::Block* block = src->getBlock(pos);
                SDK::BlockLegacy* legacy = block ? block->getLegacy() : nullptr;
                if (!legacy) continue;

                const Kind kind = classify(legacy->getName(), legacy->getBlockId());
                if (kind == Kind::None || !wantsKind(kind)) continue;

                // Interact through a face that is not buried, so the server's own
                // reach/visibility check on the click position agrees with us.
                int face = -1;
                for (const FaceProbe& probe : kFaces) {
                    const SDK::BlockPos neighbour(pos.x + probe.dx, pos.y + probe.dy, pos.z + probe.dz);
                    if (!src->isSolid(neighbour)) { face = probe.face; break; }
                }
                if (face < 0) continue; // fully enclosed — unreachable

                best.pos = pos;
                best.kind = kind;
                best.face = face;
                best.distance = dist;
            }
        }
    }

    if (best.kind == Kind::None) return false;
    *out = best;
    return true;
}

bool ChestAura::tryOpen(const SDK::GameContext& ctx, const Candidate& target) {
    void** vt = nullptr;
    void* gm = GM::resolve(ctx.lp, &vt);
    if (!gm || !vt) return false;

    // A block in hand turns the interaction into a placement, so park on an
    // empty slot for the duration and restore the player's slot afterwards.
    if (mEmptyHand->mValue && mRestoreSlot < 0) {
        const int empty = findEmptyHotbarSlot(ctx.lp);
        if (empty >= 0) {
            const int current = SDK::Inv::selectedSlot(ctx.lp);
            if (current >= 0 && current != empty) {
                mRestoreSlot = current;
                SDK::PacketUtils::sendHotbarSlot(ctx.lp, empty);
            }
        }
    }

    const SDK::IVec3 click(target.pos.x, target.pos.y, target.pos.z);
    float hx, hy, hz;
    faceHitPoint(target.face,
                 static_cast<float>(target.pos.x),
                 static_cast<float>(target.pos.y),
                 static_cast<float>(target.pos.z),
                 &hx, &hy, &hz);

    // The server validates the transaction against its own HitResult view, so
    // prime it before dispatching (identical to Scaffold's placement path). The
    // hit point sits on the exact face we send so the reach/visibility check
    // agrees and the container actually opens.
    GM::prepBlockHit(ctx.level, click, target.face, hx, hy, hz);
    if (!GM::buildBlock(gm, vt, click, static_cast<unsigned char>(target.face)))
        return false;

    GM::swing(ctx.lp, GM::SwingSource::Interact);
    return true;
}

void ChestAura::releaseSlot(SDK::Actor* lp) {
    if (mRestoreSlot < 0) return;
    if (lp) SDK::PacketUtils::sendHotbarSlot(lp, mRestoreSlot);
    mRestoreSlot = -1;
}

void ChestAura::resetState() {
    mLastTick = -1;
    mLastOpenMs = 0;
    mAwaitingScreen = false;
    mAwaitSinceMs = 0;
    mPending = SDK::BlockPos();
}

void ChestAura::OnEnable() {
    resetState();
    mVisited.clear();
    mVisitedStampMs = SDK::nowMs();
    mRestoreSlot = -1;
}

void ChestAura::OnDisable() {
    SDK::GameContext ctx;
    if (SDK::resolveContext(ctx, false)) releaseSlot(ctx.lp);
    mRestoreSlot = -1;
    resetState();
    SilentRotation::Clear(true);
}

void ChestAura::OnUpdate() {
    if (!IsEnabled()) return;

    const bool dbg = mDebug->mValue;

    SDK::GameContext ctx;
    if (!SDK::resolveContext(ctx, true)) return;

    // A container screen being up means the last open landed: record it, hand
    // the hotbar back, and idle until the player closes the UI. ChestStealer (if
    // enabled) does the looting from here.
    const std::string& screen = SDK::ScreenView::currentScreenName();
    const bool containerOpen = ChestStealer::IsContainerScreen(screen);
    if (containerOpen) {
        if (mAwaitingScreen) {
            if (mSkipVisited->mValue) mVisited.insert(mPending);
            mAwaitingScreen = false;
            if (mNotify->mValue) {
                char line[64];
                std::snprintf(line, sizeof(line), "Opened container at %d %d %d",
                              mPending.x, mPending.y, mPending.z);
                Notifications::AddNotification(line);
            }
            auraDbg(dbg, "screen '%s' confirmed open at %d %d %d",
                    screen.c_str(), mPending.x, mPending.y, mPending.z);
        }
        releaseSlot(ctx.lp);
        SilentRotation::Clear(true);
        return;
    }

    // One interaction per game tick, matching every other action module.
    const long long tick = ctx.lp->getTicksExisted();
    if (tick >= 0) {
        if (tick == mLastTick) {
            // Keep the continuous latch alive on skipped frames, or the packet
            // hook loses the rotation between ticks.
            if (mRotate->mValue && mAwaitingScreen) {
                Vec3 eye{};
                if (sehGetPos(ctx.lp, &eye)) {
                    const Vec3 aim(static_cast<float>(mPending.x) + 0.5f,
                                   static_cast<float>(mPending.y) + 0.5f,
                                   static_cast<float>(mPending.z) + 0.5f);
                    const Vec2 look = calcLookAngles(eye, aim);
                    SilentRotation::SetContinuous(look.y, look.x);
                    const bool w_ = (GetAsyncKeyState('W') & 0x8000) != 0;
                    const bool a_ = (GetAsyncKeyState('A') & 0x8000) != 0;
                    const bool s_ = (GetAsyncKeyState('S') & 0x8000) != 0;
                    const bool d_ = (GetAsyncKeyState('D') & 0x8000) != 0;
                    SilentRotation::SetMoveInput((w_ ? 1 : 0) - (s_ ? 1 : 0), (d_ ? 1 : 0) - (a_ ? 1 : 0));
                    SilentRotation::PublishMoveFrame(ctx.lp->getRot().y);
                }
            }
            return;
        }
        mLastTick = tick;
    }

    const uint64_t now = SDK::nowMs();

    // Expire the opened-container memory so a long session can loot a respawned
    // or refilled chest again.
    if (mSkipVisited->mValue && mForgetAfter->mValue > 0.f) {
        const uint64_t ttl = static_cast<uint64_t>(mForgetAfter->mValue) * 1000ull;
        if (now - mVisitedStampMs >= ttl) {
            mVisited.clear();
            mVisitedStampMs = now;
            auraDbg(dbg, "opened-list expired (%.0fs)", mForgetAfter->mValue);
        }
    }

    // The previous interaction produced no screen — treat the target as a dud so
    // one protected chest cannot stall the module forever.
    if (mAwaitingScreen && now - mAwaitSinceMs >= kAwaitScreenTimeoutMs) {
        if (mSkipVisited->mValue) mVisited.insert(mPending);
        mAwaitingScreen = false;
        releaseSlot(ctx.lp);
        auraDbg(dbg, "no screen for %d %d %d — skipping it",
                mPending.x, mPending.y, mPending.z);
    }

    SilentRotation::Clear(false);
    if (mAwaitingScreen) return;
    if (mLastOpenMs != 0 && now - mLastOpenMs < static_cast<uint64_t>(mDelay->mValue)) return;

    Candidate target;
    if (!findTarget(ctx.ci, ctx.lp, &target)) {
        releaseSlot(ctx.lp);
        return;
    }

    if (mRotate->mValue) {
        Vec3 eye{};
        if (sehGetPos(ctx.lp, &eye)) {
            const Vec3 aim(static_cast<float>(target.pos.x) + 0.5f,
                           static_cast<float>(target.pos.y) + 0.5f,
                           static_cast<float>(target.pos.z) + 0.5f);
            const Vec2 look = calcLookAngles(eye, aim);
            SilentRotation::SetContinuous(look.y, look.x);
                    const bool w_ = (GetAsyncKeyState('W') & 0x8000) != 0;
                        const bool a_ = (GetAsyncKeyState('A') & 0x8000) != 0;
                        const bool s_ = (GetAsyncKeyState('S') & 0x8000) != 0;
                        const bool d_ = (GetAsyncKeyState('D') & 0x8000) != 0;
                        SilentRotation::SetMoveInput((w_ ? 1 : 0) - (s_ ? 1 : 0), (d_ ? 1 : 0) - (a_ ? 1 : 0));
                        SilentRotation::PublishMoveFrame(ctx.lp->getRot().y);
        }
    }

    if (tryOpen(ctx, target)) {
        mPending = target.pos;
        mAwaitingScreen = true;
        mAwaitSinceMs = now;
        mLastOpenMs = now;
        auraDbg(dbg, "interact %d %d %d face=%d dist=%.2f",
                target.pos.x, target.pos.y, target.pos.z, target.face, target.distance);
    } else {
        releaseSlot(ctx.lp);
        auraDbg(dbg, "interact FAILED at %d %d %d", target.pos.x, target.pos.y, target.pos.z);
    }
}
