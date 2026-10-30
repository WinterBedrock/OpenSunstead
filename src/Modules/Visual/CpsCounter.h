#pragma once
#include "../Module.h"
#include "CounterStyle.h"
#include <vector>

class CpsCounter : public Module {
public:
    CpsCounter() : Module("CPS Counter", "Displays your Clicks Per Second", Category::Visual, 0) {
        mIsHudModule = true;
        AddSetting(new EnumSetting("Style", "Color theme style", 0, {"Theme", "Custom"}));
        AddSetting(new ColorSetting("Text Color", "Color of the suffix text", 1.0f, 1.0f, 1.0f, 1.0f));
        AddSetting(new EnumSetting("Mode", "Which clicks to show", 2, {"LMB", "RMB", "Both"}));
        AddSetting(new NumberSetting("Background Blur", "Blur intensity behind the panel", 10.0f, 0.0f, 20.0f, 0.5f));
        AddSetting(new NumberSetting("Background Dim", "Opacity of the background", 0.30f, 0.0f, 1.0f, 0.05f));
        AddSetting(new NumberSetting("Background Rounding", "Roundness of the background corners", 5.0f, 0.0f, 20.0f, 1.0f));
        AddSetting(new BoolSetting("Text Shadow", "Drop shadow behind the text", false));
        AddSetting(new NumberSetting("Shadow Opacity", "Opacity of the plate drop shadow", 0.35f, 0.0f, 1.0f, 0.05f));
        AddSetting(new BoolSetting("Gradient Numbers", "Draw the value in the theme gradient", true));
        AddSetting(new BoolSetting("Number Animation", "Animate the value when it changes", true));
    }
    
    void OnRender() override;

private:
    std::vector<double> lLmbClicks;
    std::vector<double> lRmbClicks;
    bool wasLmbDown = false;
    bool wasRmbDown = false;

    int GetCPS(std::vector<double>& clickList);
};
