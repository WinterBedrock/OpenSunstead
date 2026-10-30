#pragma once
#include "../Module.h"

namespace SDK { class Actor; }

// ESP — draws boxes around nearby players via an ImGui screen-space overlay.
// Box world-position is taken the Latite/Hitboxes way (state-vector pos +
// AABB dimensions) and projected to screen.
class ESP : public Module {
public:
    ESP() : Module("ESP", "Draws boxes around nearby players", Category::Visual, 0) {
        // [0] Box render style. "Flat": ONE vertical world-space plane through
        // the player's center, oriented by the ACTOR'S OWN look direction —
        // the strip runs across the body, presenting its face toward whatever
        // they are facing, glued to them like real 3D geometry (interp render
        // position, distance scaling).
        AddSetting(new EnumSetting("Mode", "Box style", 0, { "2D", "2D Corner", "3D", "Outline", "Flat" }));
        // [1] outline color source
        AddSetting(new EnumSetting("Color", "Box outline color source", 0, { "Theme", "Custom" }));
        // [2] custom outline color (used when Color = Custom)
        AddSetting(new ColorSetting("Custom Color", "Custom outline color", 1.0f, 0.2f, 0.2f, 1.0f));
        // [3] line thickness
        AddSetting(new NumberSetting("Thickness", "Box line thickness", 1.5f, 0.5f, 5.0f, 0.1f));
        // [4] fill toggle (on by default)
        AddSetting(new BoolSetting("Fill", "Fill the box with a translucent color", true));
        // [5] fill color source
        AddSetting(new EnumSetting("Fill Color", "Box fill color source", 0, { "Theme", "Custom" }));
        // [6] custom fill color
        AddSetting(new ColorSetting("Fill Custom", "Custom fill color", 1.0f, 0.2f, 0.2f, 0.25f));
        // [7] fill opacity (0..1)
        AddSetting(new NumberSetting("Fill Opacity", "Translucent fill alpha", 0.25f, 0.0f, 1.0f, 0.01f));
        // [8] also draw non-player entities (mobs, items, etc.)
        AddSetting(new BoolSetting("Entities", "Also show non-player entities", false));
        // [9] render outside the in-world HUD too (off by default -> only on hud_screen)
        AddSetting(new BoolSetting("Always show", "Render on every screen, not just the in-world HUD", false));
        // [10] leftover of the old AddLine-corner path; Rise CS uses w/4 h/4.
        AddSetting(new NumberSetting("Corner Length", "2D Corner arm length in px", 16.0f, 4.0f, 64.0f, 1.0f));
        // [11] also render the LOCAL player's box while the game camera is in
        // third person (0 = first person -> nothing drawn on self)
        AddSetting(new BoolSetting("Render Self", "Also show your own box when the camera is in third person", false));
        // [12] master theme switch for the box OUTLINE color: when on, the box
        // follows ColorUtils::getThemedColor() (the animated Theme / Gradient /
        // Color Speed / Saturation palette every other overlay uses) even when
        // "Color" [1] is left on Custom. APPENDED LAST on purpose: settings are
        // read by index in ESP.cpp and persisted by name, so nothing may be
        // inserted before this one.
        AddSetting(new BoolSetting("Theme", "Box outline follows the client's animated theme color", false));
        // [13] HP indicator: a vertical bar beside the box (2D) or a flat
        // health ring lying on the ground under the player (Ring, default).
        AddSetting(new BoolSetting("HP Show", "Show a health indicator on each box", true));
        // [14] HP indicator style: 0 = 2D vertical bar (Rise-style), 1 = Ring.
        AddSetting(new EnumSetting("HP Type", "Health indicator style", 1, { "2D", "Ring" }));
        // [15] HP indicator opacity (multiplies every alpha the bar/ring draws).
        AddSetting(new NumberSetting("HP Opacity", "Health indicator opacity", 1.0f, 0.05f, 1.0f, 0.05f));
        // [16] Flat mode: width multiplier on the strip (1.0 = the hitbox
        // footprint) — the plane itself is anchored to the player's world
        // position and scales with distance like real geometry.
        AddSetting(new NumberSetting("Flat Scale", "Flat plane width multiplier (anchored to the player in the world)", 1.0f, 0.5f, 2.5f, 0.05f));
        // [17] 2D modes only: the same gaussian drop shadow the menu chrome
        // (buttons, ArrayList rows) uses, hugging the OUTSIDE of the box.
        AddSetting(new BoolSetting("Shadow", "2D modes: soft shadow around the outside of the lines", false));
        // [18]/[19] Shadow tuning (visible while Shadow is on).
        AddSetting(new NumberSetting("Shadow Opacity", "How strong the box shadow is", 0.35f, 0.05f, 1.0f, 0.05f));
        AddSetting(new NumberSetting("Shadow Offset", "How far the box shadow spreads (px)", 9.0f, 1.0f, 20.0f, 1.0f));
        // [20] Expand master switch: off (default) = the boxes stay the
        // original hitbox size; the [21] slider below only shows while on.
        AddSetting(new BoolSetting("Expand", "Grow the boxes outward from the original hitbox", false));
        // [21] Expand amount in world blocks.
        AddSetting(new NumberSetting("Expand Size", "How far the boxes grow outward (blocks)", 0.05f, 0.05f, 0.15f, 0.01f));
    }

    void UpdateSettingsVisibility() override {
        // [10] "Corner Length" is a retired knob (Rise CS uses w/4 h/4).
        if (mSettings.size() > 10)
            mSettings[10]->mVisible = false;
        const int mode = static_cast<EnumSetting*>(mSettings[0])->mValue;
        // [14] HP Type / [15] HP Opacity only exist while HP Show is on.
        if (mSettings.size() > 15) {
            const bool hpOn = static_cast<BoolSetting*>(mSettings[13])->mValue;
            mSettings[14]->mVisible = hpOn;
            mSettings[15]->mVisible = hpOn;
        }
        // [17]-[19] Shadow is a 2D / 2D Corner look; its two knobs hide with it.
        if (mSettings.size() > 19) {
            const bool shadow2d = (mode <= 1) && static_cast<BoolSetting*>(mSettings[17])->mValue;
            mSettings[17]->mVisible = (mode <= 1);
            mSettings[18]->mVisible = shadow2d;
            mSettings[19]->mVisible = shadow2d;
        }
        // [21] "Expand Size" hides while the [20] Expand switch is off.
        if (mSettings.size() > 21)
            mSettings[21]->mVisible = static_cast<BoolSetting*>(mSettings[20])->mValue;
    }

    void OnEnable() override;
    void OnRender() override;
};