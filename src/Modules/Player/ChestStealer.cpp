#include "ChestStealer.h"
#include "../Setting.h"
#include "../../SDK/SDK.h"
#include "../../SDK/Client/ScreenView.h"
#include "../../SDK/Client/Chat.h"
#include "../../SDK/Client/ClientInstance.h"
#include "../../SDK/Inventory/Inventory.h"
#include "../../Core/Logger.h"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include <chrono>
#include <random>
#include <string>
#include <cstdio>
#include <sstream>
#include <cstring>
#include <imgui.h>

extern uintptr_t g_ContainerScreenController_handleAutoPlace;
extern uintptr_t g_ContainerScreenController_getSelectedSlotInfo;
extern uintptr_t g_ContainerScreenController_getItemStack;

static const std::string kContainerItems = "container_items";

// Poll cadence once the container has been emptied. Slow enough to cost nothing,
// fast enough to notice items another player drops in while we stand there.
static constexpr uint64_t kIdlePollMs = 500;

static uint64_t NowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 0 Randomize Delay, 1 Delay, 2 Min, 3 Max, 4 Close When Done, 5 Debug, 6 Show Progress, 7 Slots Per Tick
ChestStealer::ChestStealer()
    : Module("ChestStealer", "Steals items from open chests/containers", Category::Player, 0) {
    AddSetting(new BoolSetting("Randomize Delay", "Randomize delay between takes", false));
    AddSetting(new NumberSetting("Delay", "Delay between steal bursts (ms)", 50.f, 0.f, 500.f, 1.f));
    AddSetting(new NumberSetting("Randomize Min", "Min random delay (ms)", 40.f, 0.f, 500.f, 1.f));
    AddSetting(new NumberSetting("Randomize Max", "Max random delay (ms)", 100.f, 0.f, 500.f, 1.f));
    AddSetting(new BoolSetting("Close When Done", "Close after 2 full slot passes", true));
    AddSetting(new BoolSetting("Debug", "Full chest dump + per-slot logs (chat+console)", false));
    AddSetting(new BoolSetting("Show Progress", "Draw Stealing progress bar", true));
    // Default 5: at 50ms delay a 27-slot chest finishes ~270ms per pass instead of 1.35s
    AddSetting(new NumberSetting("Slots Per Tick", "How many slots to autoplace per delay tick", 5.f, 1.f, 27.f, 1.f));

    // ---- anti-desync block --------------------------------------------------
    // The old cadence fired a fixed-delay burst, moved every slot in a strict
    // 0..N order, and tryExit()'d the moment a scan said the chest was empty.
    // Depending on the response latency of the last ItemStackRequest the
    // server could still be validating the batch when it saw the close, and it
    // CANCELS / desyncs the tail slots. The controls below are only appended
    // so the positional getters above stay stable (exactly like
    // CounterStyle.h). The names must survive the Edit dump (mSettings order).
    AddSetting(new BoolSetting("Take Random Slot First", "First slot of the burst is drawn randomly (spread robustness check)", true)); // [8]
    AddSetting(new NumberSetting("Batch Size", "Ops per packet batch. Lower = closer to per-slot broadcasts, higher = fewer packet churn", 1.f, 1.f, 27.f, 1.f)); // [9]
    AddSetting(new NumberSetting("Close Delay", "Post-empty wait (ms) before tryExit — covers the last request round-trip", 150.f, 0.f, 2000.f, 1.f)); // [10]
    AddSetting(new NumberSetting("Randomization", "Extra timing jitter added on top (%)", 20.f, 0.f, 100.f, 1.f));                                     // [11]
    AddSetting(new BoolSetting("Random Order", "Shuffle the slot order every pass", true));                                                  // [12]

    canBind = true;
}

void ChestStealer::UpdateSettingsVisibility() {
    bool rnd = static_cast<BoolSetting*>(mSettings[0])->mValue;
    mSettings[1]->mVisible = !rnd;
    mSettings[2]->mVisible = rnd;
    mSettings[3]->mVisible = rnd;
}

uint64_t ChestStealer::getDelayMs() const {
    float lo = static_cast<const NumberSetting*>(mSettings[2])->mValue;
    float hi = static_cast<const NumberSetting*>(mSettings[3])->mValue;
    if (!static_cast<const BoolSetting*>(mSettings[0])->mValue) {
        lo = static_cast<const NumberSetting*>(mSettings[1])->mValue;
        hi = lo;
    }
    if (hi < lo) std::swap(lo, hi);
    static std::mt19937 rng{ std::random_device{}() };
    std::uniform_real_distribution<float> dist(lo, hi);
    float base = dist(rng);
    // Extra jitter on top (Randomization %) — a constant delay is trivially
    // fingerprinted by per-container timing; the multiplier re-draws every
    // orbit so the cadence is never a fixed frame.
    float randPct = 0.f;
    if (mSettings.size() > 11)
        randPct = static_cast<const NumberSetting*>(mSettings[11])->mValue;
    if (randPct > 0.f) {
        std::uniform_real_distribution<float> jit(1.f - randPct * 0.01f, 1.f + randPct * 0.01f);
        base *= jit(rng);
    }
    return (uint64_t)(base < 0.f ? 0.f : base);
}

ChestStealer::AntiDesyncCfg ChestStealer::getAntiDesync() const {
    AntiDesyncCfg adc;
    if (mSettings.size() > 12) {
        adc.takeRandomSlotFirst = (int)static_cast<BoolSetting*>(mSettings[8])->mValue;
        adc.batchSize           = (int)static_cast<NumberSetting*>(mSettings[9])->mValue;
        adc.minDelayMs          = static_cast<NumberSetting*>(mSettings[2])->mValue;
        adc.maxDelayMs          = static_cast<NumberSetting*>(mSettings[3])->mValue;
        adc.randomizationPct    = static_cast<NumberSetting*>(mSettings[11])->mValue;
        adc.closeDelayMs        = static_cast<NumberSetting*>(mSettings[10])->mValue;
        adc.randomizedOrder     = (int)static_cast<BoolSetting*>(mSettings[12])->mValue;
    } else {
        adc.takeRandomSlotFirst = -1;
        adc.batchSize = -1;
        adc.randomizedOrder = -1;
        adc.minDelayMs = -1.f;
        adc.maxDelayMs = -1.f;
        adc.randomizationPct = -1.f;
        adc.closeDelayMs = -1.f;
    }
    return adc;
}

int ChestStealer::getSlotsPerTick() const {
    int v = (int)static_cast<const NumberSetting*>(mSettings[7])->mValue;
    if (v < 1) v = 1;
    if (v > 27) v = 27;
    return v;
}

bool ChestStealer::isDebug() const { return static_cast<const BoolSetting*>(mSettings[5])->mValue; }
bool ChestStealer::showProgress() const { return static_cast<const BoolSetting*>(mSettings[6])->mValue; }
bool ChestStealer::closeWhenDone() const { return static_cast<const BoolSetting*>(mSettings[4])->mValue; }

bool ChestStealer::IsContainerScreen(const std::string& sn) {
    if (sn.empty()) return false;
    if (sn.find("chest") != std::string::npos) return true;
    if (sn.find("barrel") != std::string::npos) return true;
    if (sn.find("shulker") != std::string::npos) return true;
    if (sn.find("hopper") != std::string::npos) return true;
    if (sn.find("dropper") != std::string::npos) return true;
    if (sn.find("dispenser") != std::string::npos) return true;
    if (sn == "container_screen") return true;
    return false;
}

int ChestStealer::MaxSlotsForScreen(const std::string& sn) {
    // Always scan 54 for "container" so double chests aren't cut short.
    // Empty autoplace is a no-op; filled ones still steal.
    if (sn.find("hopper") != std::string::npos) return 5;
    if (sn.find("dropper") != std::string::npos || sn.find("dispenser") != std::string::npos) return 9;
    if (sn.find("small_chest") != std::string::npos || sn.find("barrel") != std::string::npos
        || sn.find("shulker") != std::string::npos) return 27;
    if (sn.find("large") != std::string::npos || sn.find("double") != std::string::npos) return 54;
    // Unknown container_screen: use 54 to not miss slots.
    return 54;
}

void ChestStealer::logAlways(const std::string& msg) {
    LOG_INFO(msg, "ChestStealer");
}

void ChestStealer::chatDebug(const std::string& msg) {
    logAlways(msg);
    if (!isDebug()) return;
    SDK::Chat::displayClientMessage("§7[CS] §f" + msg);
}

void ChestStealer::dumpChestDebug(void* cscRaw, const std::string& sn) {
    // Debug-only. Everything below touches native memory, so normal users never
    // execute any of it — the previous version ran a blind ContainerManagerModel
    // vtable probe on every chest open regardless of this setting.
    if (!isDebug()) return;

    std::ostringstream oss;
    oss << "=== CHEST DUMP screen='" << sn << "' csc=0x" << std::hex << (uintptr_t)cscRaw
        << " autoPlace=0x" << g_ContainerScreenController_handleAutoPlace
        << " getItemStack=0x" << g_ContainerScreenController_getItemStack
        << std::dec << " maxSlots=" << mMaxSlots << " ===";
    logAlways(oss.str());
    SDK::Chat::displayClientMessage("§e[CS] §fDumping chest → logs.txt");

    // Player inventory free slots (helps explain "half not stolen").
    int freeSlots = 0, invItems = 0;
    SDK::ClientInstance* ci = SDK::ClientInstance::get();
    SDK::Actor* lp = ci ? ci->getLocalPlayer() : nullptr;
    SDK::PlayerInventory* sup = lp ? lp->getSupplies() : nullptr;
    SDK::Container* inv = sup ? sup->getContainer() : nullptr;
    if (inv) {
        std::ostringstream invLines;
        for (int i = 0; i < 36; ++i) {
            SDK::ItemStack* st = inv->getItem(i);
            if (!st || !st->isValidStack()) { ++freeSlots; continue; }
            ++invItems;
            std::string iname = st->getItemName();
            if (iname.empty()) iname = "?";
            invLines << "  inv[" << i << "] count=" << st->getCount() << " name='" << iname << "'\n";
        }
        if (invItems) logAlways("player inventory:\n" + invLines.str());
    }
    {
        char line[128];
        std::snprintf(line, sizeof(line), "player inv: items=%d freeSlots~%d", invItems, freeSlots);
        logAlways(line);
        SDK::Chat::displayClientMessage(std::string("§7[CS] §f") + line);
    }

    // Chest contents come from the controller we already type-verified, not from
    // a guessed ContainerManagerModel vtable index.
    auto* csc = reinterpret_cast<SDK::ContainerScreenController*>(cscRaw);
    if (!g_ContainerScreenController_getItemStack) {
        logAlways("getItemStack unresolved — blind slot sweep 0.." + std::to_string(mMaxSlots - 1));
        SDK::Chat::displayClientMessage("§c[CS] §fNo getItemStack — blind sweep");
        mFilledGuess = mMaxSlots;
        logAlways("=== END DUMP ===");
        return;
    }

    // Only non-empty slots are logged, and the whole dump goes out as a few
    // lines instead of one file write per slot. The old per-slot version cost
    // roughly a second of stall on a 27-slot chest (a log flush and an
    // item-name offset probe for every empty slot).
    int filled = 0;
    std::ostringstream items;
    for (int i = 0; i < mMaxSlots; ++i) {
        SDK::ItemStack* st = csc->getItemStack(kContainerItems, i);
        if (!st || !st->isValidStack()) continue;
        ++filled;
        std::string iname = st->getItemName();
        if (iname.empty()) iname = "?";
        items << "  chest[" << i << "] count=" << st->getCount() << " name='" << iname << "'\n";
    }
    mFilledGuess = filled;
    if (filled) logAlways("filled slots:\n" + items.str());
    char sum[96];
    std::snprintf(sum, sizeof(sum), "scan: filled=%d / %d", filled, mMaxSlots);
    logAlways(sum);
    SDK::Chat::displayClientMessage(std::string("§e[CS] §f") + sum);
    logAlways("=== END DUMP ===");
}

void ChestStealer::OnEnable() {
    mLastStealMs = 0;
    mSlot = 0;
    mPass = 0;
    mOpenMs = 0;
    mCloseAfterMs = 0;
    mLastSn.clear();
    mStealing = false;
    mClosed = false;
    mDumpedThisOpen = false;
    mRejectedLogged = false;
    mFinished = false;
    mEmptyTicks = 0;
    mStolenThisOpen = 0;
    mCallsThisOpen = 0;
    logAlways("ON");
    chatDebug("ON autoPlace=0x" + std::to_string(g_ContainerScreenController_handleAutoPlace));
}

void ChestStealer::OnDisable() {
    mStealing = false;
    mClosed = false;
    logAlways("OFF stolen=" + std::to_string(mStolenThisOpen) + " calls=" + std::to_string(mCallsThisOpen));
}

void ChestStealer::OnContainerFrame(void* csc, const std::string& screenName) {
    if (!IsEnabled()) return;
    trySteal(csc, screenName);
}

void ChestStealer::OnUpdate() {
    // No steal here — only UI thread (OnContainerFrame). Avoid double-fire races.
    if (!IsEnabled()) return;
    const std::string& sn = SDK::ScreenView::currentScreenName();
    if (!IsContainerScreen(sn)) {
        if (!mLastSn.empty()) {
            logAlways("left container '" + mLastSn + "' stolen=" + std::to_string(mStolenThisOpen)
                + " calls=" + std::to_string(mCallsThisOpen) + " slot=" + std::to_string(mSlot)
                + " pass=" + std::to_string(mPass));
        }
        mLastSn.clear();
        mStealing = false;
        mClosed = false;
        mDumpedThisOpen = false;
        mRejectedLogged = false;
    mFinished = false;
        mEmptyTicks = 0;
        mSlot = 0;
        mPass = 0;
        mCloseAfterMs = 0;
    }
}

void ChestStealer::trySteal(void* cscRaw, const std::string& sn) {
    if (!IsContainerScreen(sn)) return;
    if (!g_ContainerScreenController_handleAutoPlace) {
        chatDebug("FATAL: handleAutoPlace=0");
        return;
    }
    // Without the family marker we cannot prove the controller's type, and a
    // wrong guess is an uncatchable __fastfail inside handleAutoPlace rather than
    // a catchable fault. Refuse to steal instead of gambling.
    if (!g_ContainerScreenController_getSelectedSlotInfo) {
        chatDebug("FATAL: CSC type marker unresolved — refusing to call handleAutoPlace");
        return;
    }
    if (mClosed) return;

    // THE fix: the screen name only tells us what the UI is called. This asks the
    // object itself whether it is really a ContainerScreenController. Chest-named
    // screens whose controller is some other class (and container screens that are
    // still being torn down) are now skipped instead of crashing the game.
    //
    // The reason is logged ONCE per container open. The old gate
    // (`mLastSn == sn`) was structurally dead: mLastSn is only assigned after
    // this check passes, so on the very first frame of the very first chest it
    // was always empty, the reject line never fired, and ChestStealer produced
    // zero output — indistinguishable from "the module does nothing".
    if (!mRejectedLogged) {
        SDK::ContainerScreenController::RejectReason why =
            SDK::ContainerScreenController::RejectReason::kAccepted;
        SDK::ContainerScreenController::CscProbe probe;
        const bool ok = SDK::ContainerScreenController::isContainerScreenController(cscRaw, &why, &probe);
        if (!ok) {
            mRejectedLogged = true;
            char line[320];
            std::snprintf(line, sizeof(line),
                "refuse '%s': ctrl=0x%p %s (vt=0x%p vt[172]=0x%llX slot=%d marker=0x%llX autoPlace=0x%llX)",
                sn.c_str(), cscRaw,
                SDK::ContainerScreenController::rejectReasonText(why),
                probe.vtable, (unsigned long long)probe.vt172, probe.markerSlot,
                (unsigned long long)g_ContainerScreenController_getSelectedSlotInfo,
                (unsigned long long)g_ContainerScreenController_handleAutoPlace);
            logAlways(line);
            // If the controller is simply null this frame (screen mid-teardown),
            // stay quiet in chat but keep the log line; clear the flag so a real
            // controller later still gets its own one-shot report.
            if (why == SDK::ContainerScreenController::RejectReason::kNullController)
                mRejectedLogged = false;
            if (isDebug())
                SDK::Chat::displayClientMessage(std::string("§c[CS] §f") + line);
        }
        if (!ok) return;
        // One-time note when the marker sits at a non-base slot — this is the
        // subclass case the old fixed-index check rejected outright.
        if (probe.markerSlot != 172) {
            char line[192];
            std::snprintf(line, sizeof(line),
                "accepted '%s': ctrl=0x%p family marker at vtable[%d] (base uses 172)",
                sn.c_str(), cscRaw, probe.markerSlot);
            logAlways(line);
        }
    } else {
        if (!SDK::ContainerScreenController::isContainerScreenController(cscRaw)) return;
    }

    const uint64_t now = NowMs();
    if (mLastSn != sn) {
        mLastSn = sn;
        mOpenMs = now;
        mSlot = 0;
        mPass = 0;
        mLastStealMs = 0;
        mStealing = true;
        mClosed = false;
        mDumpedThisOpen = false;
        mRejectedLogged = false;
        mFinished = false;
        mStolenThisOpen = 0;
        mCallsThisOpen = 0;
        mEmptyTicks = 0;
        mMaxSlots = MaxSlotsForScreen(sn);
        chatDebug("opened '" + sn + "' maxSlots=" + std::to_string(mMaxSlots)
            + " delay=" + std::to_string(getDelayMs())
            + "ms burst=" + std::to_string(getSlotsPerTick()));
    }

    // Once the container is empty, drop to a slow idle poll instead of the steal
    // cadence. Standing in an emptied chest used to cost a full 27-slot native
    // scan plus a log line every 50 ms — the looting stutter. Still polls, so
    // items dropped in by someone else are picked up.
    if (mFinished) {
        // Close When Done + Close Delay: once the container reads empty and
        // the post-empty countdown has passed, exit. Until then we idle-poll —
        // no more native scans than needed on a static screen.
        if (closeWhenDone() && mCloseAfterMs > 0 && now >= mCloseAfterMs) {
            auto* cx = reinterpret_cast<SDK::ContainerScreenController*>(cscRaw);
            if (cx) cx->tryExit();
            mClosed = true;
            mStealing = false;
            chatDebug("closeDelay done — tryExit (calls=" + std::to_string(mCallsThisOpen) + ")");
            mCloseAfterMs = 0;
        }
        if (now - mLastStealMs < kIdlePollMs) return;
    }

    if (!mDumpedThisOpen && now - mOpenMs >= 150) {
        mDumpedThisOpen = true;
        dumpChestDebug(cscRaw, sn);
    }

    if (now - mOpenMs < 200) return;
    if (mLastStealMs != 0 && now - mLastStealMs < getDelayMs()) return;

    auto* csc = reinterpret_cast<SDK::ContainerScreenController*>(cscRaw);
    const int burst = getSlotsPerTick();
    const bool canSeeItems = g_ContainerScreenController_getItemStack != 0;

    if (canSeeItems) {
        // ONE scan per tick. The previous version scanned twice (once to steal,
        // once to count what was left) and kept scanning forever after the chest
        // was already empty — 54 native calls plus a log write every 50 ms, which
        // is exactly the stutter seen while looting.
        int filled[54];
        int nFilled = 0;
        for (int i = 0; i < mMaxSlots && nFilled < 54; ++i) {
            SDK::ItemStack* st = csc->getItemStack(kContainerItems, i);
            if (st && st->isValidStack()) filled[nFilled++] = i;
        }
        mFilledGuess = nFilled;

        if (nFilled == 0) {
            mLastStealMs = now;
            if (mFinished) return;              // already reported; stay in idle poll
            if (++mEmptyTicks < 2) return;      // one empty look can be a pending ack

            // CloseDelay arm: the LAST request just landed visually empty
            // (the server may not have ACK'd yet); hand the chest its
            // post-empty grace period before tryExit. Without this the server
            // could still be validating the tail batch when we close and it
            // CANCELS / desyncs the last slots.
            mFinished = true;
            mStealing = false;
            const AntiDesyncCfg adc = getAntiDesync();
            float extra = adc.closeDelayMs > 0.f ? adc.closeDelayMs : 150.f;
            mCloseAfterMs = now + (uint64_t)extra;
            chatDebug("done — closeDelay=" + std::to_string((int)extra)
                      + "ms before exit (calls=" + std::to_string(mCallsThisOpen) + ")");
            return;
        }

        // Something is in there (again) — resume the normal steal cadence.
        mEmptyTicks = 0;
        mFinished = false;
        mCloseAfterMs = 0;

        // ---- slot order and batching (anti-desync) --------------------------
        // Strict 0..N in lockstep cadence is trivially fingerprinted, and a
        // single 27-batch handed to the ItemStackRequest queue the same tick
        // had the server rejecting mid-batch on high-throughput worlds.
        // Settings (all appended; pick them up by name):
        //   Batch Size        ops per burst before we wait another delay tick.
        //   Take Random Slot First  the first tick opens on a random slot, so
        //                     the leading slot is not always index 0.
        //   Random Order      freshly re-shuffle the filled slot list every
        //                     pass, so consecutive passes never have the same
        //                     cadence or order.
        //   Randomization     ±% jitter on this burst's delay (per-decode).
        // This does NOT change the underlying semantics of any single move; the
        // server sees them in a randomized but still self-consistent sequence.
        const AntiDesyncCfg adc = getAntiDesync();
        static std::mt19937 rng{ std::random_device{}() };
        const bool shuffleAll = adc.randomizedOrder > 0;
        const bool firstRandom = adc.takeRandomSlotFirst > 0;
        int orderBuf[54];
        if (nFilled <= (int)(sizeof(orderBuf)/sizeof(orderBuf[0])))
            std::memcpy(orderBuf, filled, (size_t)nFilled * sizeof(int));
        else {
            // Should never happen (mMaxSlots clamped to 54) but keep scope
            std::memcpy(orderBuf, filled, sizeof(orderBuf));
        }
        if (shuffleAll && nFilled > 1) {
            for (int i = nFilled - 1; i > 0; --i) {
                std::uniform_int_distribution<int> di(0, i);
                int j = di(rng);
                int t = orderBuf[i];
                orderBuf[i] = orderBuf[j];
                orderBuf[j] = t;
            }
        } else if (firstRandom && nFilled > 1) {
            // With Random Order off but Take Random Slot First on, draw only the
            // head of the pass — pieces preserved otherwise, so the pass still
            // streams linearly.
            std::uniform_int_distribution<int> di(0, nFilled - 1);
            int j = di(rng);
            int t = orderBuf[0];
            orderBuf[0] = orderBuf[j];
            orderBuf[j] = t;
        }

        // Batch Size gate: the packet sender sees <Batch Size> autoplaces per
        // burst — never the full pass — which prevents the ItemStackRequest
        // queue from bursting on the server in a way it then rejects mid-batch.
        const int batchCap = adc.batchSize > 0 ? adc.batchSize : 1;
        const int effective = std::min(burst, batchCap);

        for (int b = 0; b < effective && b < nFilled; ++b) {
            const int slot = orderBuf[b];
            csc->handleAutoPlace(kContainerItems, slot);
            ++mCallsThisOpen;
            ++mStolenThisOpen;
            mSlot = slot;
            if (isDebug()) {
                char line[128];
                std::snprintf(line, sizeof(line), "autoPlace slot=%d/%d burst=%d/%d batch=%d calls=%d",
                    slot, mMaxSlots, b + 1, burst, batchCap, mCallsThisOpen);
                logAlways(line);
            }
        }
    } else {
        // Fallback when getItemStack is unavailable: the original blind sweep.
        for (int b = 0; b < burst; ++b) {
            if (mSlot < 0 || mSlot >= mMaxSlots) mSlot = 0;

            csc->handleAutoPlace(kContainerItems, mSlot);
            ++mCallsThisOpen;
            ++mStolenThisOpen;

            if (isDebug()) {
                char line[128];
                std::snprintf(line, sizeof(line),
                    "autoPlace slot=%d/%d pass=%d burst=%d/%d calls=%d",
                    mSlot, mMaxSlots, mPass, b + 1, burst, mCallsThisOpen);
                logAlways(line);
                if ((mSlot % 9) == 0)
                    SDK::Chat::displayClientMessage(std::string("§7[CS] §f") + line);
            }

            mSlot++;
            if (mSlot >= mMaxSlots) {
                mSlot = 0;
                mPass++;
                chatDebug("pass complete #" + std::to_string(mPass)
                    + " calls=" + std::to_string(mCallsThisOpen));
                if (closeWhenDone() && mPass >= 2) {
                    csc->tryExit();
                    mClosed = true;
                    mStealing = false;
                    chatDebug("tryExit once after 2 passes");
                    mPass = 0;
                    break;
                }
            }
        }
    }

    mLastStealMs = now;
    if (mClosed) mStealing = false;
}

void ChestStealer::OnRender() {
    if (!IsEnabled() || !showProgress() || !mStealing) return;
    if (!IsContainerScreen(SDK::ScreenView::currentScreenName())) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    const float sw = ImGui::GetIO().DisplaySize.x;
    const float sh = ImGui::GetIO().DisplaySize.y;
    const float boxW = 240.f, boxH = 58.f;
    const float x = (sw - boxW) * 0.5f;
    const float y = sh * 0.18f;
    ImColor accent = ColorUtils::getThemedColor(0.f);

    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + boxW, y + boxH),
        ImGui::GetColorU32(ImVec4(0.06f, 0.06f, 0.08f, 0.88f)), 8.f);
    dl->AddRect(ImVec2(x, y), ImVec2(x + boxW, y + boxH),
        ImGui::GetColorU32(ImVec4(accent.Value.x, accent.Value.y, accent.Value.z, 0.55f)), 8.f, 0, 1.5f);

    const bool itemAware = g_ContainerScreenController_getItemStack != 0;
    float p;
    char title[80];
    if (itemAware) {
        // Progress against what is actually left in the container.
        const int left = mFilledGuess < 0 ? 0 : mFilledGuess;
        const int span = mMaxSlots > 0 ? mMaxSlots : 1;
        p = 1.f - (float)left / (float)span;
        if (p < 0.f) p = 0.f;
        if (p > 1.f) p = 1.f;
        std::snprintf(title, sizeof(title), "Stealing…  %d item%s left", left, left == 1 ? "" : "s");
    } else {
        p = mMaxSlots > 0 ? (float)mSlot / (float)mMaxSlots : 0.f;
        std::snprintf(title, sizeof(title), "Stealing…  %d / %d  (pass %d)", mSlot, mMaxSlots, mPass + 1);
    }
    ImRenderUtils::drawText(ImVec2(x + 12.f, y + 8.f), title, ImColor(1.f, 1.f, 1.f, 1.f), 15.f, 1.f, true, dl);

    char sub[64];
    std::snprintf(sub, sizeof(sub), "calls %d  burst %d", mCallsThisOpen, getSlotsPerTick());
    ImRenderUtils::drawText(ImVec2(x + 12.f, y + 24.f), sub, ImColor(0.7f, 0.75f, 0.85f, 1.f), 12.f, 1.f, true, dl);

    const float bx = x + 12.f, by = y + 42.f, bw = boxW - 24.f, bh = 8.f;
    dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh),
        ImGui::GetColorU32(ImVec4(0.15f, 0.15f, 0.18f, 0.9f)), 4.f);
    if (p > 0.001f)
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw * p, by + bh),
            ImGui::GetColorU32(ImVec4(accent.Value.x, accent.Value.y, accent.Value.z, 0.95f)), 4.f);
}
