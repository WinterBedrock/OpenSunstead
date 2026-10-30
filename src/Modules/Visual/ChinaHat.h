#pragma once
#include "../Module.h"

namespace SDK { class Actor; }

class ChinaHat : public Module {
public:
    ChinaHat() : Module("ChinaHat", "Draws a cone hat above players' heads", Category::Visual, 0) {
        // [0] cone radius (blocks)
        AddSetting(new NumberSetting("Radius", "Radius of the cone base", 0.5f, 0.1f, 2.0f, 0.05f));
        // [1] cone height (blocks)
        AddSetting(new NumberSetting("Height", "Height of the cone above the head", 0.35f, 0.1f, 2.0f, 0.05f));
        // [2] number of segments (smoothness)
        AddSetting(new NumberSetting("Segments", "Number of cone base segments", 32.0f, 6.0f, 64.0f, 1.0f));
        // [3] outline thickness
        AddSetting(new NumberSetting("Thickness", "Outline line thickness", 1.5f, 0.5f, 4.0f, 0.1f));
        // [4] include local player
        AddSetting(new BoolSetting("Render Others", "Also draw the hat on other players (off = only you)", false));
        // [5] cone color
        AddSetting(new ColorSetting("Color", "Cone color", 1.0f, 0.2f, 0.2f, 1.0f));
        // [6] fill opacity
        AddSetting(new NumberSetting("Fill Opacity", "Translucent fill alpha", 0.35f, 0.0f, 1.0f, 0.01f));
        // The former [2] "Offset" NumberSetting was removed. The vertical offset
        // above the head is now the file-local constant kHatOffset (0.0f — the
        // removed setting's default) in ChinaHat.cpp. Indices [2]..[6] above have
        // been shifted down by one to match.
    }

    void OnEnable() override;
    void OnRender() override;
};