#include "FogColor.h"
#include "../Setting.h"

FogColor::FogColor()
    : Module("FogColor", "Overrides the world fog colour", Category::Visual, 0) {
    // [0] Sky Color type: Custom uses the picker below, Theme uses the
    //     interface theme's gradient RGB.
    AddSetting(new EnumSetting("Sky Color", "Colour source", 0, { "Custom", "Theme" }));
    // [1] Custom colour (only used when type == Custom)
    AddSetting(new ColorSetting("Color", "Custom fog colour", 0.55f, 0.25f, 0.85f, 1.0f));
}

int   FogColor::GetMode() { return static_cast<EnumSetting*>(mSettings[0])->mValue; }
float FogColor::GetR() { return static_cast<ColorSetting*>(mSettings[1])->mValue[0]; }
float FogColor::GetG() { return static_cast<ColorSetting*>(mSettings[1])->mValue[1]; }
float FogColor::GetB() { return static_cast<ColorSetting*>(mSettings[1])->mValue[2]; }
