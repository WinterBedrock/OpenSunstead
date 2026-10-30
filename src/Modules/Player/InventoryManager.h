#pragma once
#include "../Module.h"
#include <cstdint>
#include <string>

// InventoryManager — auto-armor, best tools to hotbar, blocks out of the
// hotbar, drop trash. Acts while the player's inventory screen is open.
//
// HOW ITEMS MOVE (reversed on 1.26.50.04)
// ---------------------------------------
// The old version shift-clicked through ContainerScreenController::
// handleAutoPlace. That is the game's "quick move" and it decides the
// destination itself: armor sometimes landed in the hotbar instead of the
// armor slot, a worn piece was never swapped, "inventory_items" does not even
// exist as a collection name on this build (so tools could never move), and a
// stale drag/selection state on the controller silently turned calls into
// no-ops. Combined with a 3 s settle window per retry that is exactly the
// "random delay, sometimes moves, armor never equips" behaviour.
//
// Now every move is an EXPLICIT request on the screen's slot model
// (ItemStackNetManagerScreen, the object at CSC+0x12C0, vtable 0x14E87F760):
//   vt[14] place(src {collection,slot}, count, dst {collection,slot})
//          -> sub_1433E7250(type=Place) — one ItemStackRequest Place action
//   vt[21] swap(src, dst) -> sub_1433E7250(type=Swap) (both slots occupied)
//   vt[22] requestDrop(src, {mode,amount,flag})
// These are the same builders the screen's own drag/drop uses, so the game's
// ItemStackNetManager owns request ids and client prediction. The slots are
// resolved relative to requestDrop's slot in the model's real vtable and each
// entry's prologue is checked before use (a wrong build -> module idles).
//
// Upgrade = ONE swap (inventory piece <-> worn piece). Empty armor slot = ONE
// place. Tool to hotbar = place into an empty hotbar slot, or swap with the
// worst non-tool hotbar slot. Each action is confirmed by watching the
// container change (fast: usually the very next frame thanks to prediction);
// an action the server rejected rolls back, earns a strike, and after a few
// strikes is blocked for this open.
class InventoryManager : public Module {
public:
    InventoryManager();

    // UI-thread entry point, dispatched from hkScreenViewSetupAndRender.
    void OnContainerFrame(void* csc, const std::string& screenName);

    void OnEnable() override;
    void OnDisable() override;
    void OnUpdate() override; // game thread: reset per-open state on close

private:
    bool getEquipArmor() const;
    bool getToolsToHotbar() const;
    bool getBlocksToInventory() const;
    bool getDropTrash() const;
    uint64_t getDelayMs() const;
    bool isDebug() const;
    std::string getTrashListRaw() const;

    // A slot address the slot model understands.
    struct SlotRef { const char* collection; int slot; };

    enum Kind { kEquip = 0, kTool = 1, kBlockOut = 2, kTrash = 3 };
    struct Candidate {
        int kind = 0;
        int srcSlot = -1;     // container slot 0..35 (source)
        int dstSlot = -1;     // container slot 0..35, or armor family 0..3 for kEquip
        bool swap = false;    // destination occupied -> swap instead of place
        int count = 1;
        int key = -1;
    };

    void resetOpenState();
    bool probeCollections(void* csc, void* lp);
    bool resolveModelActions(void* model);
    bool pickCandidate(void* lp, Candidate& out);
    bool issue(void* csc, const Candidate& c);
    SlotRef containerRef(int containerSlot, int variant) const;
    SlotRef armorRef(int family) const;

    bool isKeyBlocked(int key) const;
    void blockKey(int key);
    void dumpRegistries(void* csc);

    // ---- per-open state ----
    bool mProbed = false;
    uint64_t mProbeMs = 0;
    int  mProbeTries = 0;
    bool mCombinedOk = false;    // combined_hotbar_and_inventory_items[i] == container slot i
    bool mHotbarOk = false;      // hotbar_items[i] == container slot i (0..8)
    int  mArmorUiSlot[4] = { 0, 1, 2, 3 };
    bool mWarnedThisOpen = false;
    bool mActionsWarned = false;

    bool mRegDumped = false;
    int mDumpTries = 0;
    uint64_t mLastDumpMs = 0;

    // Resolved slot-model entry points (cached per model vtable).
    void** mModelVt = nullptr;
    uintptr_t mPlaceFn = 0;
    uintptr_t mSwapFn = 0;
    uintptr_t mDropFn = 0;

    // ---- action pacing / confirmation ----
    uint64_t mLastActionMs = 0;
    int  mPendingKey = -1;           // action waiting for confirmation
    uint64_t mPendingMs = 0;
    std::string mPendingSrcName;     // source slot name when issued
    int  mPendingSrc = -1;
    int  mPendingVariant = 0;        // which collection mapping was used
    static constexpr uint64_t kConfirmMs = 1200;
    static constexpr int kMaxStrikes = 4;
    int  mStrikeKey[24] = { 0 };
    int  mStrikeCnt[24] = { 0 };
    int  mStrikeN = 0;
    int  mBlocked[32] = { 0 };
    int  mBlockedCount = 0;
    int  strikesFor(int key) const;
    int  addStrike(int key);
};
