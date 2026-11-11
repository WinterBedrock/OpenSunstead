#pragma once
#include "../Module.h"

namespace SDK { class Actor; }

class Hat : public Module {
public:
    Hat() : Module("Hat", "Draws a hat above players' heads", Category::Visual, 0) {
        AddSetting(new EnumSetting("Type", "Hat style", 1, { "Cone", "Halo" }));
        AddSetting(new NumberSetting("Radius", "Radius of the hat base", 0.5f, 0.1f, 2.0f, 0.05f));
        AddSetting(new NumberSetting("Height", "Cone height (halo: float height above the head)", 0.35f, 0.1f, 2.0f, 0.05f));
        AddSetting(new NumberSetting("Segments", "Number of base segments", 32.0f, 6.0f, 64.0f, 1.0f));
        AddSetting(new NumberSetting("Thickness", "Outline line thickness", 1.5f, 0.5f, 4.0f, 0.1f));
        AddSetting(new BoolSetting("Render Others", "Also draw the hat on other players (off = only you)", false));
        AddSetting(new ColorSetting("Color", "Cone color", 1.0f, 0.2f, 0.2f, 1.0f));
        AddSetting(new NumberSetting("Fill Opacity", "Translucent fill alpha (cone) / glow strength (halo)", 0.35f, 0.0f, 1.0f, 0.01f));
        AddSetting(new BoolSetting("Tilt With Head", "Tilt the hat when the head pitches up/down", true));
    }

    bool IsHalo() const { return static_cast<EnumSetting*>(mSettings[0])->mValue == 1; }

    void UpdateSettingsVisibility() override {
        const bool cone = !IsHalo();
        mSettings[6]->mVisible = cone;
    }

    void OnEnable() override;
    void OnRender() override;
};
