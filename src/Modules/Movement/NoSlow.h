#pragma once
#include "../Module.h"

class NoSlow : public Module {
public:
    NoSlow() : Module("NoSlow", "Removes the eating / item-use movement slowdown", Category::Movement, 0) {}

    // Queried by the ItemUseSlowdownApply detour in GameHooks.cpp: true when this
    // module is enabled (i.e. the slowdown should be cancelled).
    static bool ShouldCancel();
};
