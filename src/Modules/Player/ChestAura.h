#pragma once
#include "../Module.h"
#include "../../SDK/Client/GameContext.h"
#include "../../SDK/World/World.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// ChestAura — opens containers within range without looking at them.
//
// An "open" is the same thing the vanilla client sends when you right-click a
// chest: the Level HitResult is primed as a BLOCK hit on the container, then
// GameMode::buildBlock issues the ItemUse/Place transaction for that position.
// The server runs Block::use for the target and answers with ContainerOpen.
// That is exactly the pair Scaffold places blocks with, so it needs no new
// signatures.
//
// Because a block in hand would place instead of interact, the module can swap
// to an empty hotbar slot for the interaction and swap back afterwards.
// ---------------------------------------------------------------------------
class ChestAura : public Module {
public:
    ChestAura();

    void OnEnable() override;
    void OnDisable() override;
    void OnUpdate() override;
    void UpdateSettingsVisibility() override;

private:
    // Container kinds the module is willing to open.
    enum class Kind { None, Chest, TrappedChest, EnderChest, Barrel, ShulkerBox };

    struct Candidate {
        SDK::BlockPos pos;
        Kind  kind = Kind::None;
        int   face = 1;
        float distance = 0.f;
    };

    // --- Settings -----------------------------------------------------------
    NumberSetting* mRange       = nullptr;
    NumberSetting* mDelay       = nullptr;
    BoolSetting*   mSkipVisited = nullptr;
    NumberSetting* mForgetAfter = nullptr;
    BoolSetting*   mChests      = nullptr;
    BoolSetting*   mTrapped     = nullptr;
    BoolSetting*   mEnderChests = nullptr;
    BoolSetting*   mBarrels     = nullptr;
    BoolSetting*   mShulkers    = nullptr;
    BoolSetting*   mRotate      = nullptr;
    BoolSetting*   mEmptyHand   = nullptr;
    BoolSetting*   mNotify      = nullptr;
    BoolSetting*   mDebug       = nullptr;

    template <typename T, typename... Args>
    T* add(Args&&... args) {
        T* setting = new T(std::forward<Args>(args)...);
        AddSetting(setting);
        return setting;
    }

    // --- Runtime state ------------------------------------------------------
    // Containers we already opened, so the module does not reopen the same one
    // the moment the screen closes. Cleared on dimension-scale position jumps
    // and by the Forget After timer.
    std::unordered_set<SDK::BlockPos, SDK::BlockPosHash> mVisited;
    uint64_t  mVisitedStampMs = 0;
    long long mLastTick   = -1;
    uint64_t  mLastOpenMs = 0;
    // Set the moment we fire an interaction; cleared once a container screen is
    // actually seen, so a refused interaction cannot wedge the module.
    bool          mAwaitingScreen = false;
    uint64_t      mAwaitSinceMs   = 0;
    SDK::BlockPos mPending;
    int           mRestoreSlot = -1;

    bool  wantsKind(Kind kind) const;
    Kind  classify(const std::string& blockName, uint16_t legacyId) const;
    float rangeBlocks() const;

    bool findTarget(SDK::ClientInstance* ci, SDK::Actor* lp, Candidate* out) const;
    bool tryOpen(const SDK::GameContext& ctx, const Candidate& target);
    void releaseSlot(SDK::Actor* lp);
    void resetState();
};
