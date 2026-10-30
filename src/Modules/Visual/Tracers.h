#pragma once
#include "../Module.h"

namespace SDK { class Actor; }

class Tracers : public Module {
public:
    Tracers() : Module("Tracers", "Draws lines toward nearby players", Category::Visual, 0) {
        AddSetting(new EnumSetting("Mode", "Tracer style", 0, { "Lines", "Arrows" }));
        AddSetting(new EnumSetting("Anchor", "Where lines originate", 2, { "Top", "Center", "Bottom" }));
        AddSetting(new EnumSetting("Color", "Line color source", 0, { "Theme", "Custom" }));
        AddSetting(new ColorSetting("Custom Color", "Custom line color", 1.0f, 0.2f, 0.2f, 1.0f));
        AddSetting(new NumberSetting("Thickness", "Line thickness", 1.5f, 0.5f, 5.0f, 0.1f));
        AddSetting(new BoolSetting("Behind View", "Also draw tracers to players behind you (clamped to screen edge)", true));
        // [5] render outside the in-world HUD too (off by default -> only on hud_screen)
        AddSetting(new BoolSetting("Always show", "Render on every screen, not just the in-world HUD", false));
        // [6] also react to non-player entities (mobs/animals)
        AddSetting(new BoolSetting("Mobs", "Also draw tracers to non-player entities", false));
    }
    void OnRender() override;
};