#pragma once
#include "../Module.h"
#include <array>
#include <string>

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
    unsigned long long mLingerMs = 0;

    std::string mLastName;
    void*       mHeadTex   = nullptr;
    std::string mHeadName;
    int         mHeadTryCd = 0;

    float mHitPulse = 0.0f;
    bool  mWasHurting = false;       // hurt-state latch for the rising edge
    void* mSeenTarget = nullptr;     // reset hurt tracking when the target changes
    unsigned long long mHurtSinceMs = 0;      // when the current hurt run began
    unsigned long long mStallPulseMs = 0;     // last stall-recovered pulse

    float mFollowX = 0.0f;
    float mFollowY = 0.0f;
    float mFollowVelX = 0.0f;
    float mFollowVelY = 0.0f;
    bool  mFollowInit = false;
    float mOnPlayerBlend = 0.0f; // 0 = hud-editor pos, 1 = attached to player

    std::string mShownName;        // name drawn on the previous frame
    float       mSwitchAge = 1e9f; // seconds since the last identity switch
};
