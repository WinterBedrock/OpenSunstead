#pragma once
#include "../Module.h"

class Jetpack : public Module {
public:
    Jetpack() : Module("Jetpack", "Lets you fly in the direction you're looking", Category::Movement, 0) {
        AddSetting(new NumberSetting("Horizontal Speed", "Horizontal flight speed", 1.0f, 0.5f, 10.0f, 0.1f));
        AddSetting(new NumberSetting("Vertical Speed", "Vertical flight speed", 1.0f, 0.5f, 10.0f, 0.1f));
        AddSetting(new BoolSetting("Reset On Disable", "Reset velocity on disable", false));
        AddSetting(new EnumSetting("Bind Type", "How the keybind works", 1, {"Toggle", "Hold"}));
        // Bypass: server-safe flight profile. Velocity is clamped into the band
        // vanilla elytra/glide produces (server motion checks expect glide-class
        // vertical drift, not rocket-class acceleration) and the outgoing
        // PlayerAuthInput is made self-consistent (glide/jump bits) so the
        // server's motion prediction never disagrees with the displacement.
        AddSetting(new BoolSetting("Bypass", "Server-safe glide-band flight (slower, but not flagged as vertical-motion abuse)", false));
        AddSetting(new NumberSetting("Bypass Glide", "Downward glide per tick in Bypass mode (vanilla elytra ~= -0.03..-0.06)", -0.05f, -0.08f, -0.01f, 0.01f));
        mHoldMode = true; // Default to hold mode
    }

    void OnTick() override;
    void OnDisable() override;
};
