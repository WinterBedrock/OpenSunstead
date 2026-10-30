#pragma once
#include "../Module.h"
#include "../../SDK/Types.h"
#include "../../SDK/World/World.h"
#include <string>
#include <unordered_map>

// Scaffold — v2 auto-block-placement (auto-bridge / tower).
//
// Placement pipeline (per pass, Speed = passes/sec up to 100):
//   resolve ctx -> resolve GameMode -> find block slot -> walk the look-ahead
//   run (facing vector + lead) -> for every AIR cell with a SOLID NEIGHBOUR:
//   prime HitResult as BLOCK on that neighbour + face, then buildBlock.
//
// NO PHYSICAL PLAYER MOVEMENT — the module only PLACES blocks. The player is
// never pushed, teleported or velocity-written; nothing here makes the player
// fall off the bridge. Head rotation is a separate, intentional feature
// (the "Rotation" setting) and is NOT movement.
//
// Server-safe rules (v2):
//   * NEIGHBOUR-ONLY placement. A cell with no solid neighbour is SKIPPED, not
//     forced: the transaction packet must name a real block to click, and
//     servers/anticheats cancel placements that click air. Cells placed
//     earlier in the same pass count as solid so a run chains block-on-block.
//   * Swing per mode: Default = local GameMode::swing() (function, animates
//     the arm); Silent = AnimatePacket (Swing) only, the local swing state is
//     never touched so the first-person animation cannot dip.
//
// Vtable ground truth (1.26.50.04): buildBlock = 7, getPickRange = 10,
// attack = 15 (see GameMode.h). The GameMode object is resolved on the local
// player at runtime and validated by the mPlayer back-reference (gm+0x8 ==
// localPlayer) — the SAME deterministic discriminator the repaired Aura uses,
// so we can never dispatch on a wrong object. Every native read/call is
// SEH-guarded.
class Scaffold : public Module {
public:
    Scaffold() : Module("Scaffold", "Automatically places blocks beneath you", Category::Movement, 0) {
        // "Build" = auto-pick a placeable block from the inventory before placing.
        // When OFF, only the currently-held item is used and if it is not a block
        // nothing gets placed (it does not disable the placement itself).
        AddSetting(new BoolSetting("Build", "Auto-select a placeable block from the hotbar before placing", true));  // [0]
        // Bridge = probe direction: facing the player's walk direction (bridge)
        // vs straight-down tower. Unrelated to Build.
        // Speed: placement passes per second (100 = every frame, no gate).
        AddSetting(new NumberSetting("Speed", "Placement passes per second. Higher = faster bridging.", 15.0f, 1.0f, 100.0f, 1.0f)); // [1]
        // Look-ahead distance in blocks. Raise if you outrun the bridge sprinting.
        AddSetting(new NumberSetting("Blocks", "Blocks laid AHEAD per pass (look-ahead). Raise for sprint bridging.", 2.0f, 1.0f, 32.0f, 1.0f)); // [2]
        AddSetting(new BoolSetting("Rotation", "Turn the player's head toward the block being placed", true));                      // [3]
        AddSetting(new BoolSetting("Show Blocks", "Draw the remaining block count as a themed HUD counter", true));                 // [4]
        AddSetting(new BoolSetting("Debug", "Debug", false));                        // [5]
        // How the HUD counter counts remaining blocks.
        AddSetting(new EnumSetting("Count Mode", "What the block counter shows", 0,
            { "Hotbar Total", "Held Slot" }));                                                                                     // [6]
        // Render the held block's TEXTURE inside the counter plate, to the LEFT
        // of the "Blocks" label (the texture is drawn by the ImGui pass on top
        // of the plate/blur — it is no longer a slot outside the panel).
        AddSetting(new BoolSetting("Block Icon", "Show the held block's texture inside the plate, left of the label", true));       // [7]
        // Silent: no visible head snap — the placement rotation is injected into
        // the outgoing PlayerAuthInput packet only (the server validates the
        // block-use against it), while the first-person camera stays untouched.
        // "Default" turns the actual player head like vanilla bridging does.
        AddSetting(new EnumSetting("Type", "Placement mode", 0, { "Default", "Silent" }));                                         // [8]
        // ---- counter-style HUD settings (see CounterStyle.h) -----------------
        // APPENDED after every setting above ON PURPOSE: the getters index
        // mSettings positionally, so new settings may only ever grow the END of
        // the list — inserting or reordering one would silently re-point them.
        // OnRender resolves all of these BY NAME.
        AddSetting(new BoolSetting("Text Shadow", "Draw a drop shadow behind the counter glyphs (off = crisp text)", false));       // [9]
        AddSetting(new NumberSetting("Shadow Opacity", "Opacity of the counter plate's drop shadow (0..1)", 0.28f, 0.0f, 1.0f, 0.02f)); // [10]
        AddSetting(new NumberSetting("Background Blur", "Backdrop blur strength behind the counter plate", 3.0f, 0.0f, 20.0f, 0.5f));     // [11]
        AddSetting(new NumberSetting("Background Dim", "Counter plate fill opacity (0..1)", 0.38f, 0.0f, 1.0f, 0.02f));            // [12]
        AddSetting(new NumberSetting("Background Rounding", "Counter plate corner radius (px)", 6.0f, 0.0f, 20.0f, 1.0f));        // [13]
        AddSetting(new BoolSetting("Gradient Numbers", "Draw the block count in the live theme gradient", true));                   // [14]
        AddSetting(new BoolSetting("Number Animation", "Roll changed digits instead of snapping", true));                           // [15]
        // Bridge: probe direction. True = look-ahead run along the facing vector
        // (cross-bridge), False = straight-down tower. Rotation settings follow.
        AddSetting(new BoolSetting("Bridge", "Probe direction: facing (bridge) instead of straight down (tower)", true));            // [16]
        // Rotation speed (deg/sec) and per-tick travel cap (deg).
        AddSetting(new NumberSetting("Rotation Speed", "Max head travel per second (degrees). Applied in aim steps.", 620.0f, 60.0f, 1440.0f, 10.0f));    // [17]
        AddSetting(new NumberSetting("Max Angle Per Tick", "Caps each aim step to this angle; 0 = uncapped", 0.0f, 0.0f, 30.0f, 1.0f));                   // [18]
        AddSetting(new EnumSetting("Smooth Mode", "Linear = constant speed. EaseOut = fast start, decelerating.", 1, { "Linear", "EaseOut" }));           // [19]
        AddSetting(new BoolSetting("Predict Placement", "Bias aim toward the placement surface (block face normal)", true));                                 // [20]
        // Micro-jitter: ±jitter deg added to each smooth step (never >2.5 - stays
        // within Bedrock sensitivity quantisation, indistinguishable from a hand).
        AddSetting(new NumberSetting("Jitter", "Micro-jitter (degrees) added per aim step; 0 = off", 2.5f, 0.0f, 2.5f, 0.5f));                               // [21]
        // Clutch (Horion tryClutchScaffold): while falling, expand the probe
        // search from a straight-facing walk to a 5^3 candidate area sorted by
        // distance. The placements still pass the same human aim gate.
        AddSetting(new BoolSetting("Clutch", "While falling: wider 5x5x5 candidate search", true));
        AddSetting(new BoolSetting("Y Spoof", "Report the bridge line height to the server while bridging (prevents movement rewinds)", true));       // [23]
        // Diagonal assist: the footing emergency also probes the cell the
        // player is ABOUT to step into (velocity lead). Diagonal sprints cross
        // into the next cell before the current one is air, so the emergency
        // must fire one cell early there — without this, diagonal bridging
        // drops the player on defaults.
        AddSetting(new BoolSetting("Diagonal", "Diagonal bridging: probe the next cell along your movement", true));                                  // [24]

        mIsHudModule = true;
        mPosX = 24.0f;
        mPosY = 300.0f;
    }

    // "Near center" default: the HUD editor resolves fractional positions once
    // at first load (when the config has no saved position) by multiplying the
    // fractions by the screen size. See PositionSetup in ModuleManager.
    void OnFirstLayout(float screenW, float screenH) override {
        if (mPosX == 24.0f && mPosY == 300.0f) {
            mPosX = screenW * 0.5f - 60.0f;
            mPosY = screenH * 0.62f;
        }
    }

    void OnUpdate() override;
    void OnRender() override;
    // Called from the ScreenView::setupAndRender hook with that hook's
    // MinecraftUIRenderContext (the only place a live ScreenContext exists —
    // same two-pass scheme BlockInfo uses for its icon).
    void DrawGameIcon(void* mcuirc, const std::string& screenName);

    bool  GetBuild()  const { return static_cast<BoolSetting*>(mSettings[0])->mValue; }   // auto-pick block slot
    float GetSpeed()  const { return static_cast<NumberSetting*>(mSettings[1])->mValue; }
    int   GetPlaces() const { return (int)static_cast<NumberSetting*>(mSettings[2])->mValue; }

    bool  GetRotation() const { return static_cast<BoolSetting*>(mSettings[3])->mValue; }
    bool  GetShowBlocks() const { return static_cast<BoolSetting*>(mSettings[4])->mValue; }
    bool  GetDebug()  const { return static_cast<BoolSetting*>(mSettings[5])->mValue; }
    int   GetCountMode() const { return static_cast<EnumSetting*>(mSettings[6])->mValue; } // 0=hotbar total 1=held
    bool  GetBlockIcon() const { return static_cast<BoolSetting*>(mSettings[7])->mValue; }
    int   GetType()     const { return static_cast<EnumSetting*>(mSettings[8])->mValue; } // 0=default 1=silent
    bool  GetBridge() const { return static_cast<BoolSetting*>(mSettings[16])->mValue; }  // probe direction (moved to END)
    float GetRotationSpeed()    const { return static_cast<NumberSetting*>(mSettings[17])->mValue; }
    float GetMaxAnglePerTick()  const { return static_cast<NumberSetting*>(mSettings[18])->mValue; }
    int   GetSmoothMode()       const { return static_cast<EnumSetting*>(mSettings[19])->mValue; }      // 0=linear 1=easeOut
    bool  GetPredictPlacement() const { return static_cast<BoolSetting*>(mSettings[20])->mValue; }
    float GetJitter()           const { return static_cast<NumberSetting*>(mSettings[21])->mValue; }
    bool  GetClutch()           const { return static_cast<BoolSetting*>(mSettings[22])->mValue; }
    bool  GetYSpoof()           const {
        return mSettings.size() > 23 && static_cast<BoolSetting*>(mSettings[23])->mValue;
    }
    bool  GetDiagonal()         const {
        return mSettings.size() > 24 && static_cast<BoolSetting*>(mSettings[24])->mValue;
    }

    float mBlocksAnim = 0.0f;
    // Number roll transition (slide-up between the previous and the new count).
    int   mShownValue = 0;   // the count currently drawn
    int   mSlideFrom = 0;    // the count sliding out
    float mSlideT = 1.0f;    // 0..1 transition progress
    int   mBlocksShown = 0;  // live inventory count (written on render)

    static bool ConsumeSilentRot(float& yaw, float& pitch);
    static void NotifySilentRot(float yaw, float pitch);
    static void InvalidateSilentRot();
    static bool ConsumeSilentMove(float& realYaw, int& fwd, int& strafe);
    static void NotifySilentMove(float realYaw, int fwd, int strafe);

    static bool ConsumeYSpoof(float& eyeY);
    static void NotifyYBridge(float placedBlockY);
    static void ResetYBridge();

private:
    unsigned long long mLastPlace = 0;
    long long mLastTick = -1;
    unsigned long long mLastPassMs = 0;
    // Bridge level pin: the Y cell the bridge run builds at. Follows the feet
    // level while the player is grounded, freezes while airborne so a
    // sprint-jump cannot flip the probe level mid-jump (see OnUpdate).
    int  mBridgeLevel = 0;
    bool mBridgeLevelValid = false;
    int  mStillFrames = 0;   // consecutive near-still passes (grounded detect)
    // 1-second Debug telemetry accumulators (see OnUpdate).
    int  mDbgPasses = 0, mDbgPlaced = 0, mDbgSkipSolid = 0, mDbgSkipSupport = 0;
    int  mDbgSkipReach = 0, mDbgSkipRepeat = 0, mDbgBuildFail = 0;
    unsigned long long mDbgLastMs = 0;

    // Serverhold bookkeeping: the slot the server currently believes is
    // selected (Silent mode). One MobEquipment goes out only when this
    // changes — per-pass swap/restore floods were getting the placements
    // cancelled on servers.
    int  mServerSlot = -1;
    // Icon slot geometry for the block texture drawn INSIDE the plate (left of
    // the "Blocks" label) by the ImGui pass. Reported in the animated (zoomed)
    // coordinates the icon actually occupies; mIconWanted is true while it is
    // being drawn. Nothing else consumes these any more — the game-render icon
    // pass (DrawGameIcon) is a no-op.
    float mIconX = 0.f, mIconY = 0.f, mIconSize = 0.f;
    bool  mIconWanted = false;
    // Cells placed recently (client BlockSource lags the server). A placement
    // issued within the TTL is skipped by the look-ahead so passes never waste
    // their budget re-placing a block the server already accepted.
    std::unordered_map<SDK::BlockPos, unsigned long long, SDK::BlockPosHash> mRecentPlaced;

public:
    void OnEnable() override {
        mBridgeLevelValid = false;
        mServerSlot = -1;
        InvalidateSilentRot();
        ResetYBridge();
    }
    void OnDisable() override {
        mBridgeLevelValid = false;
        mServerSlot = -1;
        InvalidateSilentRot();
        ResetYBridge();
    }
};
