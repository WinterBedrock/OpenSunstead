#pragma once
#include "../Module.h"

// NoFire — removes the burning-entity flame overlay (the FlameMesh the game
// draws around on-fire actors), the attached flame particle emitters AND the
// first-person burning overlay (screen flames), which is drawn as flame
// BILLBOARD particles (the FlameBillboard hook in GameHooks). The fire STATUS
// itself is untouched (damage, sounds, other effects remain).
class NoFire : public Module {
public:
    NoFire() : Module("NoFire", "Removes the fire overlay incl. the first-person burning overlay", Category::Visual, 0) {}
};
