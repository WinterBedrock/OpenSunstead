#pragma once
#include "../Module.h"
#include "../../SDK/World/World.h"
#include <climits>
#include <vector>

class OreMiner : public Module {
public:
    enum class UncoverMode { None = 0, PathFind = 1 };

    OreMiner() : Module("OreMiner", "Automatically breaks ores", Category::Player, 0) {
        AddSetting(new EnumSetting("Uncover Mode", "None = exposed only; Path Find digs a path to buried ore", 0,
            { "None", "Path Find" }));                                                                                 // [0]
        AddSetting(new NumberSetting("Range", "Max range for destroying blocks", 5.0f, 1.0f, 10.0f, 0.1f));            // [1]
        AddSetting(new NumberSetting("Uncover Range", "Max range for uncover path search", 3.0f, 1.0f, 8.0f, 1.0f));   // [2]
        AddSetting(new NumberSetting("Rate Multiplier", "Scales the real per-tick break rate (1 = exactly what vanilla does; lower = slower, never faster)", 1.0f, 0.05f, 1.0f, 0.05f)); // [3]

        AddSetting(new BoolSetting("Emerald", "Mine emerald ore", true));                                              // [4]
        AddSetting(new BoolSetting("Diamond", "Mine diamond ore", true));                                              // [5]
        AddSetting(new BoolSetting("Gold", "Mine gold ore", false));                                                   // [6]
        AddSetting(new BoolSetting("Iron", "Mine iron ore", false));                                                   // [7]
        AddSetting(new BoolSetting("Coal", "Mine coal ore", false));                                                   // [8]
        AddSetting(new BoolSetting("Redstone", "Mine redstone ore", false));                                           // [9]
        AddSetting(new BoolSetting("Lapis", "Mine lapis ore", false));                                                 // [10]
        AddSetting(new BoolSetting("Ancient Debris", "Mine ancient debris (needs diamond+ pickaxe)", true));           // [11]

        AddSetting(new BoolSetting("Swing", "Swing arm while breaking", true));                                        // [12]
        AddSetting(new BoolSetting("Hotbar Only", "Only switch to tools in the hotbar", true));                        // [13]
        AddSetting(new BoolSetting("Rotate", "Silent-rotate toward the block being mined", true));                     // [14]
        AddSetting(new BoolSetting("Render Block", "Draw the block currently being broken", true));                    // [15]
        AddSetting(new BoolSetting("Mining Bar", "Draw the centered Mining progress bar below screen center", true));  // [16]
        AddSetting(new BoolSetting("Bar Icon", "Show the mined ore's real texture inside the plate, left of the label", true)); // [17]
        AddSetting(new BoolSetting("Debug", "Print diagnostics to the console", false));                               // [18]
        AddSetting(new NumberSetting("Background Blur", "Backdrop blur strength behind the plate", 6.0f, 0.0f, 20.0f, 0.5f)); // [19]
        AddSetting(new NumberSetting("Background Dim", "Plate fill opacity (0..1)", 0.55f, 0.0f, 1.0f, 0.02f));        // [20]
        AddSetting(new NumberSetting("Shadow Opacity", "Opacity of the plate's drop shadow (0..1)", 0.35f, 0.0f, 1.0f, 0.02f)); // [21]
        AddSetting(new NumberSetting("Background Rounding", "Plate corner radius (px)", 6.0f, 0.0f, 20.0f, 1.0f));     // [22]
        AddSetting(new BoolSetting("Text Shadow", "Draw a drop shadow behind the bar glyphs (off = crisp text)", false)); // [23]
        AddSetting(new BoolSetting("Gradient Numbers", "Draw the percentage in the live theme gradient", true));       // [24]
        AddSetting(new EnumSetting("Mode", "Destruction mode", 1, { "Default", "Silent" }));                             // [25]

        AddSetting(new BoolSetting("Show Swing", "In Silent mode, also play the local first-person swing animation (packet swing still goes out)", false)); // [26]
        AddSetting(new BoolSetting("Silent Slot Switch", "Silent mode tool switch: public serverhold (ON, recommended) or purely client-side (OFF)", true)); // [27]

        mIsHudModule = true;   // bar is draggable/scaleable in the HUD editor
        mPosX = 0.0f;
        mPosY = 0.0f;
        mScale = 1.0f;
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnUpdate() override;
    void OnRender() override;

    int   GetUncoverMode() const { return static_cast<EnumSetting*>(mSettings[0])->mValue; }
    float GetRange()       const { return static_cast<NumberSetting*>(mSettings[1])->mValue; }
    float GetUncoverRange()const { return static_cast<NumberSetting*>(mSettings[2])->mValue; }
    float GetRateMultiplier()const { return static_cast<NumberSetting*>(mSettings[3])->mValue; }
    bool  GetSwing()       const { return static_cast<BoolSetting*>(mSettings[12])->mValue; }
    bool  GetHotbarOnly()  const { return static_cast<BoolSetting*>(mSettings[13])->mValue; }
    bool  GetRotate()      const { return static_cast<BoolSetting*>(mSettings[14])->mValue; }
    bool  GetRenderBlock() const { return static_cast<BoolSetting*>(mSettings[15])->mValue; }
    bool  GetMiningBar()   const { return static_cast<BoolSetting*>(mSettings[16])->mValue; }
    bool  GetBarIcon()     const { return static_cast<BoolSetting*>(mSettings[17])->mValue; }
    bool  GetDebug()       const { return static_cast<BoolSetting*>(mSettings[18])->mValue; }
    // Plate style, named like the counter modules (Background Blur / Dim /
    // Rounding / Shadow Opacity).
    float GetBarBlur()     const { return static_cast<NumberSetting*>(mSettings[19])->mValue; }
    float GetBarDim()      const { return static_cast<NumberSetting*>(mSettings[20])->mValue; }
    float GetBarShadow()   const { return static_cast<NumberSetting*>(mSettings[21])->mValue; }
    float GetBarRounding() const { return static_cast<NumberSetting*>(mSettings[22])->mValue; }
    bool  GetTextShadow()  const { return static_cast<BoolSetting*>(mSettings[23])->mValue; }
    bool  GetGradientPct() const { return static_cast<BoolSetting*>(mSettings[24])->mValue; }
    // 0 = Default (local swing + plain slot flip), 1 = Silent (packet swing +
    // serverhold MobEquipment). Defaults to Silent when the setting is missing.
    int   GetMode()        const { return mSettings.size() > 25 ? static_cast<EnumSetting*>(mSettings[25])->mValue : 1; }
    bool  GetShowSwing()   const;
    bool  GetSilentSlotSwitch() const;

    // Legacy hook entry (game-side ItemRenderer): always a no-op now — the bar
    // icon is painted by the ImGui pass with the ore's real texture.
    void DrawGameIcon(void* mcuirc, const std::string& screenName);
    void renderMiningBar();
    float mBarAnim = 0.f;
    // First-render layout latch: centered default applied once, then the HUD
    // editor owns the position.
    bool mBarPlaced = false;

    // Shared with other modules that may need to know "we're mining this".
    static inline SDK::BlockPos s_currentBlock{ INT_MAX, INT_MAX, INT_MAX };
    static inline bool s_isMining = false;

private:
    void reset();
    void ensureServerHoldsTool(SDK::Actor* lp);
    void restoreSlot(SDK::Actor* lp);
    void queueBlock(const SDK::BlockPos& pos, int face);
    bool isValidOre(const SDK::BlockPos& pos, bool oreOnly, bool exposedOnly) const;
    bool isOreName(const std::string& name, int kind) const; // kind 0..6
    int  getExposedFace(const SDK::BlockPos& pos) const;
    int  findBestPickaxe(SDK::Actor* lp) const;
    bool getBestPathToBlock(const SDK::BlockPos& ore, SDK::BlockPos* out) const;

    SDK::BlockPos mCurrentBlock{ INT_MAX, INT_MAX, INT_MAX };
    SDK::BlockPos mTargettingBlock{ INT_MAX, INT_MAX, INT_MAX };
    int   mCurrentFace = -1;
    float mBreakingProgress = 0.f;
    float mLastRate = 0.f;   // most recent per-tick rate, for the debug line
    bool  mIsMining = false;
    bool  mIsUncovering = false;
    bool  mShouldRotate = false;
    int   mToolSlot = -1;
    int   mPrevSlot = -1;
    int   mServerSlot = -1;
    // Last observed visible selection (Silent): a change means the game sent
    // its own MobEquipment, so the server no longer holds the tool.
    int   mLastLocalSlot = -1;
    long long mLastTick = -1;
    // Real elapsed seconds between game ticks (default 0.05 = vanilla tick).
    float mTickDelta = 0.05f;
    ULONGLONG mLastTickTimeMs = 0;
    // Consecutive ticks with an unusable break rate -> drop the target.
    int   mStallTicks = 0;

    // The games own threshold
    static constexpr float kBreakDone = 0.99998f;

    // Render lerp
    float mRenderProgress = 0.f;
    // Target-transition glide: the drawn box chases the queued block with an
    // exponential ease instead of teleporting when a new ore is picked.
    Vec3  mRenderCenter{ 0.f, 0.f, 0.f };
    SDK::BlockPos mLastQueuedBlock{ INT_MAX, INT_MAX, INT_MAX };
    float mRenderLerp = 1.f;
    bool  mRenderHavePos = false;
    float mBarIconX = 0.f, mBarIconY = 0.f, mBarIconSize = 0.f;
    bool  mBarIconWanted = false;
};
