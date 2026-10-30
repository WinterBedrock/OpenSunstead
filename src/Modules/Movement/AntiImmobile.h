#pragma once
#include "../Module.h"
#include "../Setting.h"
#include <cstdint>

// AntiImmobile — keeps the player movable while a server holds them frozen
// (spawn cages, countdowns, anti-cheat holds, hub lobbies).
//
// The client's "is this entity immobile" predicate is sub_141ED7610 (the function
// behind the doc string "Returns true if the subject entity is immobile. An entity
// is immobile if it lacks AI goals, has just changed dimensions or if it is a mob
// and has no health"). Reversing it on 1.26.42.01 shows it answers true on TWO
// independent grounds, so clearing only one of them leaves you frozen:
//
//   * ActorDataFlagComponent (entt type_hash 0xC67426F3) has bit 16 set —
//     ActorFlags::Noai. The check is a literal `flagsWord0 & 0x10000` and it
//     short-circuits to "immobile" on both of the predicate's code paths.
//   * the entity carries ActorIsImmobileFlagComponent (entt type_hash
//     0xAB4E40CB). The predicate tests that sparse set directly (via
//     sub_141ED7CE0) and returns true on a hit REGARDLESS of the Noai bit.
//
// When the predicate is true the movement integrator skips the whole travel-vector
// update, so no input can move you, and a second system zeroes the velocity.
//
// This module therefore clears BOTH every game tick: the Noai bit in place, and
// the tag component through the game's own Actor::setImmobile(false) so entt's
// bookkeeping stays consistent. Both are purely client-side — no packet is sent,
// and a server that re-applies the freeze simply gets it undone next tick.
//
// Three bugs kept the previous build inert, all fixed here: the tag clear ran on
// the render thread instead of the game thread (this codebase dispatches OnUpdate
// from hkUpdatePlayer and OnTick from renderLevel — the opposite of what the old
// comments assumed); the Noai write was gated on g_entt_assure_actorDataFlag even
// though its working path uses the generic g_entt_assure; and OnEnable bailed out
// on that same wrong signature.
class AntiImmobile : public Module {
public:
    AntiImmobile();

    void OnEnable() override;
    void OnDisable() override;
    void OnUpdate() override;
    void OnTick() override;

private:
    bool notify() const;
    bool clearTag() const;

    // Shared clear routine. allowTagClear restricts the entt structural remove of
    // the immobile tag to the GAME thread (OnUpdate); the render pass (OnTick)
    // passes false and only does the idempotent in-place Noai bit flip.
    void clearNow(bool allowTagClear);

    uintptr_t mTagAssureFn = 0;  // assure<ActorIsImmobileFlagComponent>, from setImmobile's body
    uint64_t  mNoaiClears  = 0;  // ticks on which Noai was actually found set
    uint64_t  mTagClears   = 0;  // ticks on which the cached tag was found set
    bool      mWarned      = false;
    bool      mReported    = false;
    bool      mStateLogged = false;
};
