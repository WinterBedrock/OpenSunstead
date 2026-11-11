#pragma once
#include "../Module.h"

class FogColor : public Module {
public:
    FogColor();
    int   GetMode();
    float GetR();
    float GetG();
    float GetB();
};
