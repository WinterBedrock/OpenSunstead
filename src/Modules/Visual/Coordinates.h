#pragma once
#include "../Module.h"
#include "CounterStyle.h"

class Coordinates : public Module {
public:
    Coordinates() : Module("Coordinates", "Displays your current XYZ coordinates", Category::Visual, 0) {
        mIsHudModule = true;
        mPosX = 10.0f;
        mPosY = 90.0f;
        AddSetting(new ColorSetting("Text Color", "Color of the suffix text", 1.0f, 1.0f, 1.0f, 1.0f));
        AddSetting(new NumberSetting("Background Blur", "Blur intensity behind the panel", 10.0f, 0.0f, 20.0f, 0.5f));
        AddSetting(new NumberSetting("Background Dim", "Opacity of the background", 0.30f, 0.0f, 1.0f, 0.05f));
        AddSetting(new NumberSetting("Background Rounding", "Roundness of the background corners", 5.0f, 0.0f, 20.0f, 1.0f));
        AddSetting(new BoolSetting("Text Shadow", "Drop shadow behind the text", false));
        AddSetting(new NumberSetting("Shadow Opacity", "Opacity of the plate drop shadow", 0.35f, 0.0f, 1.0f, 0.05f));
        AddSetting(new BoolSetting("Gradient Numbers", "Draw the value in the theme gradient", true));
        AddSetting(new BoolSetting("Number Animation", "Animate the value when it changes", true));
        AddSetting(new NumberSetting("Decimals", "Coordinate decimal places (0 = rounded)", 0.0f, 0.0f, 3.0f, 1.0f));
    }
    
    void OnRender() override;
};
