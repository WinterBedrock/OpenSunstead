#include "TimeChanger.h"
#include "../Setting.h"

TimeChanger::TimeChanger()
    : Module("TimeChanger", "Locks the world time of day", Category::Visual, 0) {
    // 0 = midnight, 6000 = noon, 18000 = midnight again (Bedrock daytime ticks)
    AddSetting(new NumberSetting("Time", "World time in ticks (0-24000)", 6000.0f, 0.0f, 24000.0f, 100.0f));
}

float TimeChanger::GetTime() {
    return static_cast<NumberSetting*>(mSettings[0])->mValue;
}
