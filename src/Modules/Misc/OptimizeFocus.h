#pragma once
#include "../Module.h"

// OptimizeFocus — when the game window loses focus (alt-tabbed), throttles the
// render thread to a low FPS cap to save GPU/CPU, and draws a dim overlay with
// a centered "window not focused" message. Restores instantly on refocus.
class OptimizeFocus : public Module {
public:
    OptimizeFocus() : Module("OptimizeFocus", "Saves resources when the game is unfocused", Category::Misc, 0) {
        AddSetting(new NumberSetting("FPS Cap", "Frame cap while unfocused", 15.0f, 1.0f, 60.0f, 1.0f));
        AddSetting(new BoolSetting("Dim", "Darken the screen while unfocused", true));
        AddSetting(new NumberSetting("Dim Amount", "Overlay darkness (0-1)", 0.6f, 0.0f, 1.0f, 0.05f));
        AddSetting(new BoolSetting("Text", "Show 'not focused' label", true));
    }

    void OnRender() override;
};
