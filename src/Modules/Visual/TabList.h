#pragma once
#include "../Module.h"
#include <Windows.h>
#include <unordered_map>
#include <string>
#include <utility>

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

    float mAnim = 0.0f;
};
