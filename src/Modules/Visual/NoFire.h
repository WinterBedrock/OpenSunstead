#pragma once
#include "../Module.h"

class NoFire : public Module {
public:
    NoFire() : Module("NoFire", "Removes the fire overlay incl. the first-person burning overlay", Category::Visual, 0) {}
};
