#pragma once
#include "../Module.h"

// Velocity — modifies the knockback the server applies to you.
//   * Cancel: the SetEntityMotion packet is cancelled outright (no knockback).
//   * Percent (default): the motion vector is scaled by the H/V multipliers.
//   * Delay: the packet is HELD — the motion is applied locally only after
//     the configured delay, so the hit registers but the shove arrives late
//     (useful to desync anti-cheat combos / wait out a combo). An optional
//     HUD readout shows the queued knockback and the milliseconds left; its
//     whole look (scale, blur, dim, rounding, bar height, glow, 3D shading)
//     is configurable, TargetHUD-style, and only shows in Delay mode.
class Velocity : public Module {
public:
    Velocity() : Module("Velocity", "Modifies your velocity from attacks, explosions, etc.", Category::Player, 0) {
        // Mode indexes are load-bearing for old configs (they are saved by
        // index): 0 was "Full" and is now "Cancel" (same cancel-the-packet
        // behaviour, clearer name), 2 stays Delay. Default flips to Percent.
        AddSetting(new EnumSetting("Mode", "The mode of velocity", 1, { "Cancel", "Percent", "Delay" }));            // [0]
        AddSetting(new NumberSetting("Horizontal", "Horizontal velocity multiplier", 0.0f, -2.0f, 2.0f, 0.01f));   // [1]
        AddSetting(new NumberSetting("Vertical", "Vertical velocity multiplier", 0.0f, -2.0f, 2.0f, 0.01f));       // [2]
        // ---- Delay mode knobs (appended; indexes above must not move) ----
        AddSetting(new NumberSetting("Delay", "Delay mode: milliseconds to hold the knockback before applying it", 250.0f, 0.0f, 1500.0f, 10.0f));  // [3]
        AddSetting(new BoolSetting("Delay Visual", "Delay mode: show the queued knockback and the time left", true));                              // [4]
        // ---- Delay visual look (TargetHUD-style knobs) ----
        AddSetting(new NumberSetting("Scale", "Delay visual: size of the indicator", 1.25f, 0.6f, 2.5f, 0.05f));                                   // [5]
        AddSetting(new NumberSetting("Blur", "Delay visual: backdrop blur strength", 0.35f, 0.0f, 1.0f, 0.05f));                                   // [6]
        AddSetting(new NumberSetting("Dim", "Delay visual: plate darkening", 0.30f, 0.0f, 1.0f, 0.05f));                                           // [7]
        AddSetting(new NumberSetting("Rounding", "Delay visual: corner rounding of the plate", 10.0f, 0.0f, 24.0f, 1.0f));                          // [8]
        AddSetting(new NumberSetting("Bar Height", "Delay visual: thickness of the timer bar", 9.0f, 4.0f, 20.0f, 1.0f));                            // [9]
        AddSetting(new BoolSetting("Bar Glow", "Delay visual: accent glow spilling out of the bar", true));                                        // [10]
        AddSetting(new BoolSetting("3D Bar", "Delay visual: shaded glossy surface on the bar", true));                                              // [11]
        AddSetting(new BoolSetting("Debug", "Log every SetEntityMotion decision to logs.txt", false));                                              // [12]
        // Appended after Debug so [0..12] stay stable.
        AddSetting(new BoolSetting("Queue", "Delay mode: remember every knockback that arrives while one is already pending (instead of replacing it)", true)); // [13]
        AddSetting(new BoolSetting("Sequential", "Queue: apply held knockbacks ONE AT A TIME — each starts counting its delay only after the previous fired", false)); // [14]
        UpdateSettingsVisibility();
    }

    // Mode-conditional rows: Horizontal/Vertical only make sense in Percent,
    // the Delay knobs only in Delay, and the look knobs only while the visual
    // itself is on. The trailing "Visible" (ArrayList) toggle injected by
    // ModuleManager is never touched here.
    void UpdateSettingsVisibility() override {
        const int  mode   = static_cast<EnumSetting*>(mSettings[0])->mValue;
        const bool percent = (mode == 1);
        const bool delay   = (mode == 2);
        mSettings[1]->mVisible = percent;  // Horizontal
        mSettings[2]->mVisible = percent;  // Vertical
        mSettings[3]->mVisible = delay;    // Delay
        mSettings[4]->mVisible = delay;    // Delay Visual
        const bool visual = delay && static_cast<BoolSetting*>(mSettings[4])->mValue;
        for (int i = 5; i <= 11; ++i)
            if (mSettings.size() > (size_t)i) mSettings[i]->mVisible = visual;
        // Queue lives inside Delay mode; Sequential only matters when Queue is on.
        if (mSettings.size() > 13) mSettings[13]->mVisible = delay;
        if (mSettings.size() > 14) mSettings[14]->mVisible = delay && mSettings[13]->mVisible;
    }

    void OnPacketReceive(class SDK::Packet* packet, int packetId, bool& cancel) override;
    void OnTick() override;    // applies due delayed knockbacks (game thread)
    void OnRender() override;  // the delay readout
    void OnDisable() override; // drop the queue

    // Queued delayed knockback (at most a couple are ever alive; the module
    // clears stale entries aggressively).
    struct PendingKB {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        unsigned long long dueMs = 0;       // GetTickCount64 when it applies
        unsigned long long queuedMs = 0;     // for the visual's bar span
    };
    std::vector<PendingKB> mPending;
};
