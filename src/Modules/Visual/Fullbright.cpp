#include "Fullbright.h"
#include "../../Hooks/GameHooks.h"
#include <Windows.h>
#include <cstdint>

float Fullbright::GetGamma() const {
    return static_cast<NumberSetting*>(mSettings[0])->mValue;
}
