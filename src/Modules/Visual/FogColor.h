#pragma once
#include "../Module.h"

// Overrides the world fog/sky colour by hooking Dimension::getSkyColor.
class FogColor : public Module {
public:
    FogColor();
    int   GetMode();   // 0 = Custom (picker), 1 = Theme (interface gradient)
    float GetR();
    float GetG();
    float GetB();
};
