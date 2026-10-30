#pragma once
#include "../Module.h"
#include <Windows.h>
#include <unordered_map>
#include <string>
#include <utility>

// TabList — a clean, centered list of every player currently connected to the
// server. Ported from the Latite hud/TabList feature, rebuilt on the Sunstead
// ImGui + D3D11 stack.
//
//   * Players are enumerated through Level::getPlayerList() which returns the
//     server-side player-list map (UUID -> PlayerListEntry). This contains ALL
//     connected players, not just nearby actors.
//   * Bind Type mirrors Jetpack/Zoom: "Hold" only draws while the keybind is
//     held; "Toggle" draws the whole time the module is enabled (incl. when it
//     was toggled from the ClickGUI).
//   * When the player count exceeds "Rows Per Column" the list wraps into
//     multiple side-by-side columns (like Latite) so everyone always fits.
//   * Player head icons are decoded from Actor::getSkin()->mSkinImage by
//     SkinUtils (face + hat overlay) and uploaded as real D3D11 textures,
//     cached per player name and pruned each frame.
class TabList : public Module {
public:
    TabList() : Module("TabList", "Lists the players in the world", Category::Visual, VK_TAB) {
        AddSetting(new EnumSetting("Bind Type", "How the keybind works", 1, {"Toggle", "Hold"}));      // [0]
        AddSetting(new BoolSetting("Player Heads", "Draw each player's head icon", true));               // [1]
        AddSetting(new BoolSetting("Background", "Draw a frosted background panel", true));              // [2]
        AddSetting(new NumberSetting("Blur", "Background blur strength", 4.0f, 0.0f, 12.0f, 0.5f));      // [3]
        AddSetting(new NumberSetting("Opacity", "Background opacity", 0.45f, 0.0f, 1.0f, 0.01f));        // [4]
        AddSetting(new NumberSetting("Text Size", "Row text size", 17.0f, 8.0f, 32.0f, 0.5f));           // [5]
        AddSetting(new NumberSetting("Max Players", "Maximum rows to display", 60.0f, 8.0f, 240.0f, 1.0f));// [6]
        AddSetting(new NumberSetting("Rows Per Column", "Rows before wrapping to a new column", 16.0f, 4.0f, 40.0f, 1.0f)); // [7]
        AddSetting(new BoolSetting("Highlight Self", "Tint your own row", true));                        // [8]
        AddSetting(new NumberSetting("Rounding", "Corner rounding of the background panel", 8.0f, 0.0f, 24.0f, 0.5f)); // [9]
        mHoldMode = true; // default Bind Type is Hold
    }

    void OnDisable() override;
    void OnRender() override;
    // Keeps mHoldMode in sync with the Bind Type enum so the keybind either
    // toggles the module (Toggle) or acts as the hold-to-show key (Hold).
    void UpdateSettingsVisibility() override;

private:
    // name -> { SRV (as void*), lastSeenFrame }. void* avoids a d3d11 include here.
    std::unordered_map<std::string, std::pair<void*, int>> mHeadTex;
    void releaseHeads();
    void syncHoldMode();

    // Open/close animation. mAnim eases toward 1 while the list should be shown
    // and toward 0 when it should hide; the panel fades + slides/scales with it
    // and only stops rendering once it is fully closed (mAnim ~ 0), giving a
    // smooth appear/disappear instead of a hard pop.
    float mAnim = 0.0f;
};
