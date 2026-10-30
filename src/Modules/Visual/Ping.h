#pragma once
#include "../Module.h"
#include "CounterStyle.h"

// Ping HUD counter — the live RTT to the server on a counter plate ("42 ms").
// The value comes from the RakPeer::GetAveragePing detour (NetPing in
// Hooks/NetworkHooks.cpp); OnTick re-polls the captured remote so the display
// updates between the game's own, much rarer polls.
class Ping : public Module {
public:
    Ping() : Module("Ping", "Displays the current ping to the server", Category::Visual, 0) {
        mIsHudModule = true;
        mPosX = 10.0f;
        mPosY = 70.0f;
        AddSetting(new ColorSetting("Text Color", "Color of the suffix text", 1.0f, 1.0f, 1.0f, 1.0f));
        AddSetting(new NumberSetting("Background Blur", "Blur intensity behind the panel", 10.0f, 0.0f, 20.0f, 0.5f));
        AddSetting(new NumberSetting("Background Dim", "Opacity of the background", 0.30f, 0.0f, 1.0f, 0.05f));
        AddSetting(new NumberSetting("Background Rounding", "Roundness of the background corners", 5.0f, 0.0f, 20.0f, 1.0f));
        AddSetting(new BoolSetting("Text Shadow", "Drop shadow behind the text", false));
        AddSetting(new NumberSetting("Shadow Opacity", "Opacity of the plate drop shadow", 0.35f, 0.0f, 1.0f, 0.05f));
        AddSetting(new BoolSetting("Gradient Numbers", "Draw the value in the theme gradient", true));
        AddSetting(new BoolSetting("Number Animation", "Animate the value when it changes", true));
    }

    void OnTick() override;
    void OnRender() override;
};
