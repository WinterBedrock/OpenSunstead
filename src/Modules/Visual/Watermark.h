#pragma once
#include "../Module.h"

class Watermark : public Module {
public:
    Watermark() : Module("Watermark", "Displays the client watermark on screen", Category::Visual, 0) {
        mIsHudModule = true;
        mScale = 2.4f;
        // A little in from the corners (the stock Module default is (10,10)).
        mPosX = 18.0f;
        mPosY = 14.0f;
        AddSetting(new EnumSetting("Color Mode", "Color mode of the watermark", 0, {"Theme", "Custom", "Rainbow"})); // [0]
        AddSetting(new BoolSetting("Glow", "Draw glow behind the watermark", true));                                 // [1]
        AddSetting(new NumberSetting("Colors", "Amount of custom colors", 3.0f, 1.0f, 6.0f, 1.0f));                   // [2]
        AddSetting(new ColorSetting("Color 1", "First custom color", 1.0f, 0.0f, 0.0f, 1.0f));                         // [3]
        AddSetting(new ColorSetting("Color 2", "Second custom color", 1.0f, 0.5f, 0.0f, 1.0f));                        // [4]
        AddSetting(new ColorSetting("Color 3", "Third custom color", 1.0f, 0.8f, 0.0f, 1.0f));                         // [5]
        AddSetting(new ColorSetting("Color 4", "Fourth custom color", 0.0f, 1.0f, 0.0f, 1.0f));                        // [6]
        AddSetting(new ColorSetting("Color 5", "Fifth custom color", 0.0f, 0.0f, 1.0f, 1.0f));                         // [7]
        AddSetting(new ColorSetting("Color 6", "Sixth custom color", 0.5f, 0.0f, 1.0f, 1.0f));                         // [8]
        AddSetting(new NumberSetting("Glow Strength", "Glow alpha strength multiplier", 0.5f, 0.1f, 1.0f, 0.05f));     // [9]
        AddSetting(new BoolSetting("Show Info", "Display extra information like FPS", false));                        // [10]
        AddSetting(new NumberSetting("Text Shadow", "Drop-shadow strength behind the text (0 = off)", 0.0f, 0.0f, 1.0f, 0.05f)); // [11]

        OnTick();
    }

    void OnRender() override;

    void OnTick() override {
        EnumSetting* colorMode = static_cast<EnumSetting*>(mSettings[0]);
        NumberSetting* colorsNum = static_cast<NumberSetting*>(mSettings[2]);

        bool isCustom = (colorMode->mValue == 1); // Custom
        colorsNum->mVisible = isCustom;

        int numColors = isCustom ? static_cast<int>(colorsNum->mValue) : 0;
        for (int i = 1; i <= 6; i++) {
            mSettings[2 + i]->mVisible = (isCustom && i <= numColors);
        }
    }
};
