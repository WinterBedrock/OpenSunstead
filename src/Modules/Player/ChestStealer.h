#pragma once
#include "../Module.h"
#include <cstdint>
#include <string>
#include <vector>

class ChestStealer : public Module {
public:
    ChestStealer();
    void OnEnable() override;
    void OnDisable() override;
    void OnUpdate() override;
    void OnRender() override;
    void UpdateSettingsVisibility() override;

    void OnContainerFrame(void* csc, const std::string& screenName);

    // Substring test over the raw screen id ("chest_screen", "large_chest",
    // "barrel", …). Public because ChestAura needs the same answer to tell
    // whether the container it just interacted with actually opened.
    static bool IsContainerScreen(const std::string& sn);

private:
    uint64_t mLastStealMs = 0;
    int mSlot = 0;
    int mPass = 0;
    int mMaxSlots = 27;
    int mStolenThisOpen = 0;
    int mCallsThisOpen = 0;
    uint64_t mOpenMs = 0;
    std::string mLastSn;
    bool mStealing = false;
    bool mClosed = false;
    bool mDumpedThisOpen = false;
    // One-shot log guard so a container screen whose controller is not a
    // ContainerScreenController does not spam the log every frame.
    bool mRejectedLogged = false;
    // Consecutive "container looks empty" observations before we exit, so a
    // mid-transaction gap does not close the chest early.
    int mEmptyTicks = 0;
    // Latched once the container is cleared. Without this the module kept
    // re-scanning all 27 slots (54 native calls) and re-logging "done" every
    // 50 ms for as long as the chest stayed open — that was the looting stutter.
    bool mFinished = false;

    // Close countdown: the game rejects a ContainerClose that happens before
    // the server has acknowledged the last ItemStackRequest, so when
    // Close When Done is on we do NOT immediately tryExit — we wait out
    // mCloseAfterMs, then exit. This is the verbose fix for "server cancels /
    // desyncs on the last slots".
    uint64_t mCloseAfterMs = 0;

    // Progress for UI / debug
    int mFilledGuess = 0;

    // ---- New anti-desync settings (resolved by name, appended to the end) --
    struct AntiDesyncCfg {
        int   randomizedOrder = -1;        // full shuffle (0) / sequence-with-gaps (1)
        int   takeRandomSlotFirst = -1;
        float minDelayMs = -1.f;
        float maxDelayMs = -1.f;
        float randomizationPct = -1.f;
        float closeDelayMs = -1.f;
        int   batchSize = -1;
    };
    AntiDesyncCfg getAntiDesync() const;

    uint64_t getDelayMs() const;
    int getSlotsPerTick() const;
    bool isDebug() const;
    bool showProgress() const;
    bool closeWhenDone() const;
    static int MaxSlotsForScreen(const std::string& sn);
    void chatDebug(const std::string& msg);
    void logAlways(const std::string& msg);
    void dumpChestDebug(void* cscRaw, const std::string& sn);
    void trySteal(void* cscRaw, const std::string& sn);
};
