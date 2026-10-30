#pragma once
#include "../Module.h"
#include <array>
#include <string>

// TargetHUD — a HUD panel showing information about a combat target. By default
// ("Only On Aura") it mirrors the Aura module's current target; with that option
// off it falls back to the nearest player within Range. Position can be fixed
// via the HUD editor or stick to the right of the on-screen player with a
// spring-smoothed follow animation (falling back to the editor position when the
// player is behind the camera). HUD-editor managed via mIsHudModule.
class TargetHUD : public Module {
public:
    TargetHUD() : Module("TargetHUD", "Shows info about the current combat target", Category::Visual, 0) {
        mIsHudModule = true;
        mPosX = 10.0f;
        mPosY = 300.0f;
        mWidth = 190.0f;
        mHeight = 60.0f;
        AddSetting(new NumberSetting("Range", "Max distance to pick the nearest player (blocks)", 7.0f, 1.0f, 100.0f, 1.0f)); // [0]
        AddSetting(new BoolSetting("Show Health", "Draw the target's health bar", true));                                       // [1]
        AddSetting(new BoolSetting("Show Distance", "Draw the target's distance", false));                                       // [2]
        AddSetting(new BoolSetting("Anti Bot", "Ignore entities flagged by the AntiBot module", true));                         // [3]
        AddSetting(new NumberSetting("Background Blur", "Blur intensity behind the panel", 10.0f, 0.0f, 20.0f, 0.5f));           // [4]
        AddSetting(new NumberSetting("Background Dim", "Opacity of the background", 0.35f, 0.0f, 1.0f, 0.05f));                  // [5]
        AddSetting(new NumberSetting("Rounding", "Roundness of the corners", 12.0f, 0.0f, 20.0f, 1.0f));                         // [6]
        AddSetting(new ColorSetting("Accent Color", "Health bar / accent color", 0.18f, 0.72f, 0.95f, 1.0f));                   // [7]
        AddSetting(new BoolSetting("Show Head", "Draw the target's player head on the left", true));                             // [8]
        // --- Extended styling (appended so existing config indices stay valid) ---
        AddSetting(new EnumSetting("Health Style", "How the health bar is drawn", 0,
            { "Theme Gradient", "HP Color", "Segmented" }));                                                                     // [9]
        AddSetting(new BoolSetting("HP Number", "Show numeric HP over the health bar", true));                                   // [10]
        AddSetting(new BoolSetting("Border", "Draw the panel border outline", true));                                            // [11]
        AddSetting(new BoolSetting("Hit Animation", "Particle burst + glow pulse when you land a hit", true));                   // [12]
        AddSetting(new NumberSetting("Bar Height", "Thickness of the health bar", 7.0f, 3.0f, 16.0f, 1.0f));                     // [13]
        AddSetting(new BoolSetting("Custom Accent", "Use the Accent Color setting instead of the active theme", false));         // [14]
        AddSetting(new NumberSetting("Panel Width", "Minimum panel width", 190.0f, 140.0f, 360.0f, 5.0f));                       // [15]
        // --- Positioning / targeting (appended) ---
        // "Fixed" is the HUD-editor position. Only the LABEL changed when the editor
        // stopped being a module; the index is what configs store, so 0 still means
        // the same thing.
        AddSetting(new EnumSetting("Position Type", "Where the panel is placed", 1,
            { "Fixed", "Player" }));                                                                                             // [16]
        AddSetting(new BoolSetting("Only On Aura", "Only show when Aura has a target (uses Aura range)", true));                 // [17]
    }

    void OnRender() override;
    void UpdateSettingsVisibility() override;
    ~TargetHUD() override;

private:
    // Smoothed readouts for a stable, non-jittery panel.
    float mDispHealth = 20.0f;
    float mDispDist   = 0.0f;
    float mFade       = 0.0f; // 0..1 acquire/lose fade (eased show/hide)
    // Linger: wall clock of the last frame that had a target. When the target
    // is lost the panel holds for this long BEFORE the fade-out starts, so a
    // target switch or a one-frame miss doesn't blink the panel away.
    unsigned long long mLingerMs = 0;

    // ---- Fade-out / head-render support ----------------------------------
    // mLastName keeps the last real target name so the panel can keep drawing
    // its content while it eases out (the old code popped out instantly).
    std::string mLastName;
    // Cached D3D11 head texture (ID3D11ShaderResourceView*) for the current
    // target, keyed by name. Rebuilt when the target changes; retried on a
    // small cadence while the server skin is still streaming in.
    void*       mHeadTex   = nullptr;
    std::string mHeadName;
    int         mHeadTryCd = 0;

    // ---- Hit feedback (hurt-effect driven) -------------------------------
    // mHitPulse: 1.0 the instant the target ENTERS the hurt state (0 -> >0 on
    // Actor+0x19C invulnerableTime — the Mob::hurtEffects timer Latite reads
    // as getHurtTime), decays to 0. The timer decrements every tick while it
    // runs, so only the 0->>0 transition counts: exactly ONE animation per
    // real hit. Drives the head tip + red flush and the panel pulse.
    float mHitPulse = 0.0f;
    bool  mWasHurting = false;       // hurt-state latch for the rising edge
    void* mSeenTarget = nullptr;     // reset hurt tracking when the target changes
    unsigned long long mHurtSinceMs = 0;      // when the current hurt run began
    unsigned long long mStallPulseMs = 0;     // last stall-recovered pulse

    // ---- Player-follow positioning (Position Type = Player) --------------
    // Spring-smoothed top-left of the panel. mFollowInit gates the first snap
    // so we don't animate from (0,0) on the first visible frame. mOnPlayer
    // tracks whether the last projection succeeded (for soft blend back to the
    // HUD-editor fallback when the target goes behind the camera).
    float mFollowX = 0.0f;
    float mFollowY = 0.0f;
    float mFollowVelX = 0.0f;
    float mFollowVelY = 0.0f;
    bool  mFollowInit = false;
    float mOnPlayerBlend = 0.0f; // 0 = hud-editor pos, 1 = attached to player

    // ---- Target-switch transition (animated card swap) --------------------
    // When the panel's target IDENTITY changes (Aura picks a different player,
    // the nearest-player fallback swaps, self-view hands over) the card used
    // to switch instantly. mShownName remembers the name drawn last frame; a
    // difference restarts mSwitchAge and the incoming name then slides in
    // horizontally while fading in (easeOutCubic, ~250 ms). mDispHealth /
    // mDispDist are NOT snapped on a switch, so the health bar value eases
    // from the old target's read to the new one during the same window.
    std::string mShownName;        // name drawn on the previous frame
    float       mSwitchAge = 1e9f; // seconds since the last identity switch
};
