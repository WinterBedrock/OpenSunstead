#pragma once
#include "../Module.h"

class TimeChanger : public Module {
public:
    TimeChanger();
    float GetTime();   // configured time in ticks (0..24000)
};
