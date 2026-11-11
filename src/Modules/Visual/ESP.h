#pragma once
#include "../Module.h"

namespace SDK { class Actor; }

class ESP : public Module {
public:
    ESP() : Module("ESP", "Draws boxes around nearby players", Category::Visual, 0) {
        AddSetting(new EnumSetting("Mode", "Box style", 0, { "2D", "2D Corner", "3D", "Outline", "Flat" }));
        AddSetting(new EnumSetting("Color", "Box outline color source", 0, { "Theme", "Custom" }));
        AddSetting(new ColorSetting("Custom Color", "Custom outline color", 1.0f, 0.2f, 0.2f, 1.0f));
        AddSetting(new NumberSetting("Thickness", "Box line thickness", 1.5f, 0.5f, 5.0f, 0.1f));
        AddSetting(new BoolSetting("Fill", "Fill the box with a translucent color", false));
        AddSetting(new EnumSetting("Fill Color", "Box fill color source", 0, { "Theme", "Custom" }));
        AddSetting(new ColorSetting("Fill Custom", "Custom fill color", 1.0f, 0.2f, 0.2f, 0.25f));
        AddSetting(new NumberSetting("Fill Opacity", "Translucent fill alpha", 0.25f, 0.0f, 1.0f, 0.01f));
        AddSetting(new BoolSetting("Entities", "Also show non-player entities", false));
        AddSetting(new BoolSetting("Always show", "Render on every screen, not just the in-world HUD", false));
        AddSetting(new NumberSetting("Corner Length", "2D Corner arm length in px", 16.0f, 4.0f, 64.0f, 1.0f));
        AddSetting(new BoolSetting("Render Self", "Render esp on self model", false));
        AddSetting(new BoolSetting("Theme", "Box outline follows the client's animated theme color", false));
        AddSetting(new BoolSetting("HP Show", "Show a health indicator on each box", true));
        AddSetting(new EnumSetting("HP Type", "Health indicator style", 0, { "2D", "Ring" }));
        AddSetting(new NumberSetting("HP Opacity", "Health indicator opacity", 1.0f, 0.05f, 1.0f, 0.05f));
        AddSetting(new NumberSetting("Flat Scale", "Flat plane width multiplier", 1.0f, 0.5f, 2.5f, 0.05f));
        AddSetting(new BoolSetting("Shadow", "Shadow in 2d mode", false));
        AddSetting(new NumberSetting("Shadow Opacity", "How strong the box shadow is", 0.35f, 0.05f, 1.0f, 0.05f));
        AddSetting(new NumberSetting("Shadow Offset", "How far the box shadow spreads", 9.0f, 1.0f, 20.0f, 1.0f));
        AddSetting(new BoolSetting("Expand", "Grow the boxes outward from the original hitbox", false));
        AddSetting(new NumberSetting("Expand Size", "How far the boxes grow outward (blocks)", 0.05f, 0.05f, 0.15f, 0.01f));
    }

    void UpdateSettingsVisibility() override {
        if (mSettings.size() > 10)
            mSettings[10]->mVisible = false;
        const int mode = static_cast<EnumSetting*>(mSettings[0])->mValue;
        if (mSettings.size() > 15) {
            const bool hpOn = static_cast<BoolSetting*>(mSettings[13])->mValue;
            mSettings[14]->mVisible = hpOn;
            mSettings[15]->mVisible = hpOn;
        }
        if (mSettings.size() > 19) {
            const bool shadow2d = (mode <= 1) && static_cast<BoolSetting*>(mSettings[17])->mValue;
            mSettings[17]->mVisible = (mode <= 1);
            mSettings[18]->mVisible = shadow2d;
            mSettings[19]->mVisible = shadow2d;
        }
        if (mSettings.size() > 21)
            mSettings[21]->mVisible = static_cast<BoolSetting*>(mSettings[20])->mValue;
    }

    void OnEnable() override;
    void OnRender() override;
};
