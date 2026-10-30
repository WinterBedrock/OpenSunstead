#pragma once
#include "../Module.h"
#include "../../SDK/Types.h"
#include "Baritone/BaritoneWorld.h"
#include "Baritone/PathExecutor.h"
#include "Baritone/Pathfinder.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

// Baritone — goal-directed pathfinding walker built on the movement-typed
// A* (Pathfinder), the analog-input PathExecutor and the SilentRotation
// channel, mirroring the Baritone-bedrock reference client.
class Baritone : public Module {
public:
    Baritone();

    void OnEnable() override;
    void OnDisable() override;
    void OnUpdate() override;
    void OnTick() override;
    void OnRender() override;
    void UpdateSettingsVisibility() override;

    // Movement restated in the spoofed travel frame, consumed by the
    // PlayerAuthInput send hook. Pure forward: that IS the world-space
    // motion the client is about to produce, in the facing we report.
    static bool ConsumeSilentMove(float& forward, float& strafe);

    // Chat-command entry points (called from GotoCommand).
    static bool RequestGoto(int x, int y, int z, std::string* why);
    static void RequestStop();
    static std::string StatusLine();

private:
    template <typename T, typename... Args>
    T* add(Args&&... args) {
        T* s = new T(std::forward<Args>(args)...);
        AddSetting(s);
        return s;
    }

    void beginSearch();
    void stepSearch();
    void stepExecute();
    void applyTravelFacing();
    void releaseControl(const char* message);
    void clearAll();
    void say(const std::string& msg) const;

    // settings
    BoolSetting*   mRenderPath    = nullptr;
    NumberSetting* mPathWidth     = nullptr;
    BoolSetting*   mRenderNodes   = nullptr;
    NumberSetting* mMaxNodes      = nullptr;
    NumberSetting* mMaxFall       = nullptr;
    BoolSetting*   mAllowHighDrops = nullptr;
    BoolSetting*   mUseWater      = nullptr;
    BoolSetting*   mAllowDiagonal = nullptr;
    BoolSetting*   mAllowParkour  = nullptr;
    BoolSetting*   mAllowSprint   = nullptr;
    BoolSetting*   mFaceTravel    = nullptr;
    BoolSetting*   mNotify        = nullptr;
    BoolSetting*   mDebug         = nullptr;

    enum class State { Idle, Searching, Executing, Failed, Arrived };

    baritone::Pathfinder mPathfinder;
    baritone::PathExecutor mExecutor;
    std::unique_ptr<baritone::BlockSourceWorld> mWorld;
    std::shared_ptr<baritone::Goal> mGoal;
    baritone::PathOptions mOptions;
    baritone::ExecutionOptions mExecOptions;

    baritone::BlockPos mGoalPos{};
    State mState = State::Idle;
    bool mPendingRequest = false;
    baritone::BlockPos mPendingGoal{};
    int mRecalcs = 0;
    int mStuckHops = 0;
    unsigned long long mLastExecuteMs = 0;

    static std::atomic<bool>  s_silentActive;
    static std::atomic<float> s_silentFwd;
    static std::atomic<float> s_silentStrafe;

    static std::atomic<bool> s_haveRequest;
    static std::atomic<int>  s_reqX, s_reqY, s_reqZ;
    static std::atomic<bool> s_reqStop;
};
