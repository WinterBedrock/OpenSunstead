#pragma once
#include "../Module.h"

namespace SDK { class Actor; }

// jump effect.
class JumpCircles : public Module {
public:
    JumpCircles() : Module("JumpCircles", "Spawns a ground effect when you land", Category::Visual, 0) {
        AddSetting(new EnumSetting("Mode", "Landing effect style", 0, { "Circle", "Particles" }));
        AddSetting(new NumberSetting("Speed", "Growth speed multiplier (1 = Rise pace)", 1.0f, 0.25f, 4.0f, 0.25f));
        AddSetting(new NumberSetting("Lifetime", "How long the effect lasts (ms)", 8000.0f, 2000.0f, 15000.0f, 250.0f));
        AddSetting(new NumberSetting("Opacity", "Effect opacity", 0.9f, 0.05f, 1.0f, 0.05f));
        AddSetting(new NumberSetting("Thickness", "Ring line width (Circle mode)", 2.0f, 1.0f, 4.0f, 0.5f));
        AddSetting(new NumberSetting("Particles", "Particle count (Particles mode)", 24.0f, 4.0f, 64.0f, 1.0f));
        AddSetting(new BoolSetting("Themed", "Use the client theme color", true));
        AddSetting(new ColorSetting("Color", "Custom effect color", 0.35f, 0.75f, 1.0f, 1.0f));
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnRender() override;
};
