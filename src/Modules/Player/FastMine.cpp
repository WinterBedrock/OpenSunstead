#include "FastMine.h"

// FastMine is implemented entirely as a detour on GameMode::getDestroyRate
// (GameHooks.cpp, hkGetDestroyRate). The detour reads this module's multiplier
// live, so there is no per-tick module logic here — enabling/disabling the
// module simply gates the multiplier inside the hook. This translation unit
// exists so the module shows up in the ClickGUI and persists its setting like
// every other Module.
