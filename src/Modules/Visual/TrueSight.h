#pragma once
#include "../Module.h"

// TrueSight — makes actors the SERVER rendered invisible (invisibility
// effect / plugin squish) visible on OUR client again.
//
//   * every tick (game thread, same thread that processes the server's
//     SetActorData packets — a clear here wins over the packet writes),
//     every actor except the local player has its ActorFlags::Invisible bit
//     CLEARED, and HiddenWhenInvisible cleared as well, so the model AND its
//     equipment render normally;
//   * purely client-side: no packet is ever sent, the server still believes
//     the actor invisible;
//   * on disable, the flag is restored for actors whose MobEffectsComponent
//     still carries the Invisibility effect (the flag's source of truth that
//     survives our writes).
class TrueSight : public Module {
public:
    TrueSight() : Module("TrueSight", "Shows invisible actors the server hides from you", Category::Visual, 0) {
        canBind = true;
    }
    void OnTick() override;
    void OnDisable() override;
};
