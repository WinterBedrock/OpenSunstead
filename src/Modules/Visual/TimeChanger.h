#pragma once
#include "../Module.h"

// Forces a fixed world time-of-day by hooking Dimension::getTimeOfDay.
// The hook reads GetTime() (ticks) and returns it as the day fraction the
// game expects, so lighting/sky follow the chosen time.
class TimeChanger : public Module {
public:
    TimeChanger();
    float GetTime();   // configured time in ticks (0..24000)
};
