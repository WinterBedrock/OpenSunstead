#include "Baritone.h"

#include "../../Core/Logger.h"
#include "../../Render/FrameView.h"
#include "../../SDK/Client/Chat.h"
#include "../../SDK/Render/BoxOverlay.h"
#include "../../SDK/SDK.h"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../Combat/SilentRotation.h"
#include "../ModuleManager.h"
#include "../Setting.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <imgui.h>

std::atomic<bool>  Baritone::s_silentActive{ false };
std::atomic<float> Baritone::s_silentFwd{ 0.f };
std::atomic<float> Baritone::s_silentStrafe{ 0.f };
std::atomic<bool> Baritone::s_haveRequest{ false };
std::atomic<int>  Baritone::s_reqX{ 0 };
std::atomic<int>  Baritone::s_reqY{ 0 };
std::atomic<int>  Baritone::s_reqZ{ 0 };
std::atomic<bool> Baritone::s_reqStop{ false };

namespace {
// How long OnUpdate may be silent before OnTick takes over (a screen is open,
// so the per-frame hook stopped firing). One tick is 50 ms.
constexpr unsigned long long kExecuteStaleMs = 120;
constexpr int kMaxRecalcs = 4;
constexpr int kMaxStuckHops = 2;
} // namespace

Baritone::Baritone()
    : Module("Baritone", "Pathfinds and walks to a goal (.goto x y z)",
             Category::Movement, 0) {
    mRenderPath    = add<BoolSetting>("Render Path", "Draw the computed path in the world", true);
    mPathWidth     = add<NumberSetting>("Path Width", "Path line thickness", 2.5f, 1.f, 6.f, 0.5f);
    mRenderNodes   = add<BoolSetting>("Render Nodes", "Mark each path step with a dot", true);
    mMaxNodes      = add<NumberSetting>("Search Limit", "Max cells to expand before giving up (thousands)", 40.f, 5.f, 200.f, 5.f);
    mMaxFall       = add<NumberSetting>("Max Fall", "Greatest drop the path may take, in blocks", 3.f, 1.f, 8.f, 1.f);
    mAllowHighDrops = add<BoolSetting>("High Drops", "Allow the path to drop more than one block; off routes around ledges instead", true);
    mUseWater      = add<BoolSetting>("Use Water", "Swim through water and drop into it from any height", false);
    mAllowDiagonal = add<BoolSetting>("Diagonals", "Allow diagonal steps (corner-safe)", true);
    mAllowParkour  = add<BoolSetting>("Parkour", "Jump one- and two-block gaps", false);
    mAllowSprint   = add<BoolSetting>("Sprint", "Hold sprint on straight runs", true);
    mFaceTravel    = add<BoolSetting>("Silent Rotation", "Report the travel direction to the server without moving your camera", true);
    mNotify        = add<BoolSetting>("Notify", "Report path progress in chat", true);
    mDebug         = add<BoolSetting>("Debug", "Log walker state to the console", false);
    canBind = true;
    UpdateSettingsVisibility();
}

void Baritone::UpdateSettingsVisibility() {
    mPathWidth->mVisible   = mRenderPath->mValue;
    mRenderNodes->mVisible = mRenderPath->mValue;
    mMaxFall->mVisible     = mAllowHighDrops->mValue;
}

void Baritone::say(const std::string& msg) const {
    if (mNotify && mNotify->mValue) SDK::Chat::displayClientMessage(msg);
}

bool Baritone::ConsumeSilentMove(float& forward, float& strafe) {
    if (!s_silentActive.load(std::memory_order_relaxed)) return false;
    forward = s_silentFwd.load(std::memory_order_relaxed);
    strafe  = s_silentStrafe.load(std::memory_order_relaxed);
    return true;
}

bool Baritone::RequestGoto(int x, int y, int z, std::string* why) {
    auto* self = static_cast<Baritone*>(
        ModuleManager::GetInstance().GetModuleByName("Baritone"));
    if (!self) {
        if (why) *why = "module not registered";
        return false;
    }
    if (!self->IsEnabled()) self->SetEnabled(true);
    s_reqX.store(x); s_reqY.store(y); s_reqZ.store(z);
    s_reqStop.store(false);
    s_haveRequest.store(true);
    return true;
}

void Baritone::RequestStop() {
    s_reqStop.store(true);
    s_haveRequest.store(true);
}

std::string Baritone::StatusLine() {
    auto* self = static_cast<Baritone*>(
        ModuleManager::GetInstance().GetModuleByName("Baritone"));
    if (!self) return "§cBaritone is not registered";
    switch (self->mState) {
    case State::Idle:
        return "§7idle — no goal set";
    case State::Searching:
        return "§esearching… §7(" + std::to_string(self->mPathfinder.getExpandedNodeCount()) + " cells)";
    case State::Executing: {
        char b[96];
        std::snprintf(b, sizeof(b), "§awalking §7step %zu/%zu toward %d %d %d",
                      self->mExecutor.getCurrentIndex(), self->mExecutor.getPath().size(),
                      self->mGoalPos.x, self->mGoalPos.y, self->mGoalPos.z);
        return b;
    }
    case State::Failed:  return "§cno path found";
    case State::Arrived: return "§aarrived";
    }
    return "§7unknown";
}

void Baritone::OnEnable() {
    clearAll();
    say("§7[§bBaritone§7] §fready — use §b.goto <x> <y> <z>");
}

void Baritone::OnDisable() {
    releaseControl(nullptr);
    clearAll();
}

void Baritone::clearAll() {
    mPathfinder.cancel();
    mGoal.reset();
    mWorld.reset();
    mState = State::Idle;
    mPendingRequest = false;
    mRecalcs = 0;
    mStuckHops = 0;
    mLastExecuteMs = 0;
}

void Baritone::releaseControl(const char* message) {
    s_silentActive.store(false, std::memory_order_relaxed);
    SilentRotation::Clear(true);

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    SDK::Actor* lp = ci ? ci->getLocalPlayer() : nullptr;
    if (lp) mExecutor.stop(lp);
    else mExecutor.stop(nullptr);

    if (message) say(std::string("§7[§bBaritone§7] ") + message);
}

void Baritone::beginSearch() {
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    SDK::Actor* lp = ci ? ci->getLocalPlayer() : nullptr;
    SDK::BlockSource* bs = ci ? ci->getBlockSource() : nullptr;
    if (!lp || !bs) {
        mState = State::Failed;
        say("§cno world to search");
        return;
    }

    Vec3 pos{};
    if (!SDK::sehGetPos(lp, &pos)) {
        mState = State::Failed;
        return;
    }

    mWorld = std::make_unique<baritone::BlockSourceWorld>(bs);

    // Snap the goal to a standable cell in the same column: `.goto` y values
    // are routinely a block or two off the actual floor.
    baritone::BlockPos goal = mPendingGoal;
    const auto standable = [this](const baritone::BlockPos& p) {
        return baritone::MovementGenerator::canStandAt(*mWorld, p, mOptions);
    };
    if (!standable(goal)) {
        for (int r = 1; r <= 6; ++r) {
            if (standable(goal.offset(0, -r, 0))) { goal.y -= r; break; }
            if (standable(goal.offset(0, r, 0))) { goal.y += r; break; }
        }
    }
    mGoalPos = goal;
    mGoal = std::make_shared<baritone::GoalBlock>(goal);

    // Snap the start to the surface actually underfoot: standing on a slab or
    // mid-jump leaves the feet without support and the search would report
    // "no path" before taking a single step.
    int sy = (int)std::floor(pos.y - 1.62f);
    for (int probe = 0; probe < 3; ++probe) {
        const auto below = mWorld->getBlockNow(baritone::BlockPos((int)std::floor(pos.x), sy - 1, (int)std::floor(pos.z)));
        if (below.loaded && below.solid) break;
        --sy;
    }

    mOptions.maxExpandedNodes = (std::size_t)(mMaxNodes->mValue * 1000.f);
    mOptions.maxFallHeight = mAllowHighDrops->mValue ? (int)mMaxFall->mValue : 1;
    mOptions.allowWater = mUseWater->mValue;
    mOptions.allowDiagonal = mAllowDiagonal->mValue;
    mOptions.allowParkour = mAllowParkour->mValue;
    mOptions.preferSprint = mAllowSprint->mValue;

    releaseControl(nullptr);
    mPathfinder.begin(baritone::BlockPos((int)std::floor(pos.x), sy, (int)std::floor(pos.z)),
                      mGoal, mOptions);
    mState = State::Searching;
}

void Baritone::stepSearch() {
    if (!mWorld) {
        mState = State::Failed;
        return;
    }
    const auto status = mPathfinder.step(*mWorld);

    if (status == baritone::SearchStatus::Searching) {
        if (mDebug->mValue) {
            static unsigned long long last = 0;
            const unsigned long long now = GetTickCount64();
            if (now - last > 400) {
                last = now;
                char b[96];
                std::snprintf(b, sizeof(b), "searching: %zu cells expanded", mPathfinder.getExpandedNodeCount());
                LOG_INFO(b, "Baritone");
            }
        }
        return;
    }

    if (status == baritone::SearchStatus::Found || status == baritone::SearchStatus::Partial) {
        auto path = mPathfinder.getPath();
        if (path.size() > 1) {
            if (status == baritone::SearchStatus::Partial)
                say("§epartial path §7(" + std::to_string(path.size()) + " steps) — continuing on arrival");
            else {
                char b[64];
                std::snprintf(b, sizeof(b), "§apath found §7(%zu steps)", path.size());
                say(b);
            }
            mExecutor.begin(std::move(path));
            mState = State::Executing;
            mLastExecuteMs = GetTickCount64();
        } else {
            mState = State::Arrived;
            say("§aalready at the goal");
        }
    } else {
        mState = State::Failed;
        say("§cno path found");
    }
}

void Baritone::applyTravelFacing() {
    float travelYaw = 0.f;
    if (!mFaceTravel->mValue || !mExecutor.consumeTravelYaw(travelYaw)) {
        s_silentActive.store(false, std::memory_order_relaxed);
        return;
    }

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    SDK::Actor* lp = ci ? ci->getLocalPlayer() : nullptr;
    if (!lp) return;

    SilentRotation::SetSmoothing(0.35f);
    SilentRotation::SetContinuous(travelYaw, lp->getRot().x);
    s_silentFwd.store(1.0f, std::memory_order_relaxed);
    s_silentStrafe.store(0.0f, std::memory_order_relaxed);
    s_silentActive.store(true, std::memory_order_relaxed);
}

void Baritone::stepExecute() {
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    SDK::Actor* lp = ci ? ci->getLocalPlayer() : nullptr;
    if (!lp || !mWorld) {
        releaseControl("§cworld gone");
        mState = State::Idle;
        return;
    }

    mExecOptions.sprint = mAllowSprint->mValue;
    mExecOptions.faceTravel = mFaceTravel->mValue;

    const auto result = mExecutor.tick(lp, *mWorld, mExecOptions);
    mLastExecuteMs = GetTickCount64();

    switch (result) {
    case baritone::ExecutionStatus::Running:
        applyTravelFacing();
        break;

    case baritone::ExecutionStatus::Arrived:
        releaseControl("§aarrived");
        mState = State::Arrived;
        break;

    case baritone::ExecutionStatus::NoPlayer:
        releaseControl(nullptr);
        mState = State::Idle;
        break;

    case baritone::ExecutionStatus::Stuck:
    case baritone::ExecutionStatus::OffPath: {
        if (mDebug->mValue)
            LOG_INFO(std::string("executor: ") + mExecutor.getLastFailureReason(), "Baritone");

        // A hop clears sub-block lips that a replan cannot see; only replan
        // when jumping demonstrably does not help.
        if (mStuckHops < kMaxStuckHops) {
            ++mStuckHops;
            mExecutor.requestHop();
            return;
        }
        if (mRecalcs >= kMaxRecalcs) {
            releaseControl("§cstuck — giving up");
            mState = State::Failed;
            return;
        }
        ++mRecalcs;
        mStuckHops = 0;
        say("§epath blocked — recalculating");
        beginSearch();
        break;
    }
    default:
        break;
    }
}

void Baritone::OnUpdate() {
    // Requests are honoured even while disabled so `.goto` can enable us.
    if (s_haveRequest.exchange(false)) {
        if (s_reqStop.exchange(false)) {
            releaseControl("§fstopped");
            clearAll();
            return;
        }
        mPendingGoal = baritone::BlockPos(s_reqX.load(), s_reqY.load(), s_reqZ.load());
        mPendingRequest = true;
    }

    if (!IsEnabled()) {
        if (mState == State::Executing)
            releaseControl(nullptr);
        return;
    }

    if (mPendingRequest) {
        mPendingRequest = false;
        mRecalcs = 0;
        mStuckHops = 0;
        beginSearch();
        if (mState == State::Searching) {
            char b[96];
            std::snprintf(b, sizeof(b), "§7[§bBaritone§7] §fpathing to §b%d %d %d",
                          mPendingGoal.x, mPendingGoal.y, mPendingGoal.z);
            say(b);
        }
    }

    switch (mState) {
    case State::Searching:
        stepSearch();
        break;
    case State::Executing:
        stepExecute();
        break;
    default:
        if (mState == State::Executing)
            releaseControl(nullptr);
        break;
    }
}

// The per-frame OnUpdate stalls while a screen is open; re-run the executor
// from the render tick when that happens, exactly like the reference client.
void Baritone::OnTick() {
    if (!IsEnabled() || mState != State::Executing) return;
    const unsigned long long now = GetTickCount64();
    if (mLastExecuteMs == 0 || now - mLastExecuteMs > kExecuteStaleMs)
        stepExecute();
}

void Baritone::OnRender() {
    if (!IsEnabled() || !mRenderPath->mValue) return;
    if (mState != State::Executing && mState != State::Searching) return;

    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    if (!ci) return;

    float mvp[16];
    Vec3 origin{};
    if (!acquireCameraMVP(ci, mvp, origin)) return;

    const auto& path = mExecutor.getPath();
    const std::size_t from = mExecutor.getCurrentIndex();

    const ImVec2 screen = ImRenderUtils::getScreenSize();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float thickness = mPathWidth->mValue;

    // Remaining path only: the walked part is noise.
    ImVec2 prevScreen{};
    bool havePrev = false;
    for (std::size_t i = from; path.size() >= 2 && i < path.size(); ++i) {
        const Vec3 world{ (float)path[i].pos.x + 0.5f,
                          (float)path[i].pos.y + 0.05f,
                          (float)path[i].pos.z + 0.5f };
        ImVec2 s{};
        if (!worldToScreen(world, mvp, origin, screen, &s)) {
            havePrev = false;
            continue;
        }

        ImColor col = ColorUtils::getThemedColor((float)i * 12.f);
        col.Value.w = 0.9f;

        if (havePrev) dl->AddLine(prevScreen, s, col, thickness);
        if (mRenderNodes->mValue) dl->AddCircleFilled(s, thickness * 1.3f, col, 10);

        prevScreen = s;
        havePrev = true;
    }

    // Goal marker: player-sized wireframe box; disappears on arrival.
    {
        constexpr float kHalfW = 0.3f;
        constexpr float kTall  = 1.8f;
        const float cx = (float)mGoalPos.x + 0.5f;
        const float cz = (float)mGoalPos.z + 0.5f;
        const float y0 = (float)mGoalPos.y + 0.01f;
        const float y1 = y0 + kTall;

        const Vec3 c[8] = {
            { cx - kHalfW, y0, cz - kHalfW }, { cx + kHalfW, y0, cz - kHalfW },
            { cx + kHalfW, y0, cz + kHalfW }, { cx - kHalfW, y0, cz + kHalfW },
            { cx - kHalfW, y1, cz - kHalfW }, { cx + kHalfW, y1, cz - kHalfW },
            { cx + kHalfW, y1, cz + kHalfW }, { cx - kHalfW, y1, cz + kHalfW },
        };
        ImVec2 p[8];
        bool ok[8];
        for (int i = 0; i < 8; ++i)
            ok[i] = worldToScreen(c[i], mvp, origin, screen, &p[i]);

        const ImColor goalCol(0.35f, 1.0f, 0.45f, 0.9f);
        for (int ei = 0; ei < 12; ++ei) {
            const int a = SDK::BoxOverlay::kCubeEdges[ei][0];
            const int b = SDK::BoxOverlay::kCubeEdges[ei][1];
            if (!ok[a] || !ok[b]) continue;
            dl->AddLine(p[a], p[b], goalCol, thickness);
        }
    }
}
