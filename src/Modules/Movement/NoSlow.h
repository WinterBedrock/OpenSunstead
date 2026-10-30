#pragma once
#include "../Module.h"

// NoSlow — removes the movement slowdown the game applies while an item is in
// use (eating, drinking, drawing a bow, blocking with a shield). Implemented as
// a MinHook detour on ItemUseSlowdownApplySystem's per-entity apply callback in
// GameHooks.cpp; the detour consults ShouldCancel() every call, so this module
// carries no per-tick logic of its own.
class NoSlow : public Module {
public:
    NoSlow() : Module("NoSlow", "Removes the eating / item-use movement slowdown", Category::Movement, 0) {}

    // Queried by the ItemUseSlowdownApply detour in GameHooks.cpp: true when this
    // module is enabled (i.e. the slowdown should be cancelled).
    static bool ShouldCancel();
};
