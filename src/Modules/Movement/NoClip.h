#pragma once
#include "../Module.h"

// NoClip — walk/fly THROUGH blocks. Rewritten from scratch (v6) after the
// spectator-mode approach turned out not to work on this build: setting the
// client GameType to Spectator does not bypass the LOCAL movement collision
// system, so all it produced was a useless velocity glide.
//
// Working mechanism (the one every modern Bedrock client uses, ported from
// the Boost reference implementation):
//
//   * COLLAPSE THE AABB HEIGHT on enable — a zero-height box cannot intersect
//     a wall horizontally, so the game's own collision resolver stops pushing
//     the player out of blocks. Width is left ALONE: the v5 width-shrink made
//     the box thin but kept it tall, which is exactly the case that still
//     collides. Height is what matters.
//   * DRIVE POSITION DIRECTLY every tick: read WASD/Space/Shift, build a
//     world-space movement vector from the look yaw, translate the AABB by it
//     and write it back, then hand the same vector to the StateVector so the
//     game's own motion/interpolation agrees with where we just put the box.
//     This is what actually moves you — a collapsed box alone only stops the
//     ejection; without the direct write you just stand still.
//   * NO velocity-only glide: writing only StateVector::velocity left the
//     collision resolver in charge of where you end up, which is how the old
//     code became a "slide along walls" module.
//
// Restored on disable: original AABB height. Held keys are released by the
// caller (the module simply stops translating the box).
class NoClip : public Module {
public:
    NoClip() : Module("NoClip", "Walk through blocks (WASD to move, SPACE/SHIFT for up/down)", Category::Movement, 0) {
        AddSetting(new NumberSetting("Horizontal", "Horizontal movement speed", 1.0f, 0.2f, 11.0f, 0.1f)); // [0]
        AddSetting(new NumberSetting("Vertical", "Vertical movement speed", 0.5f, 0.2f, 5.0f, 0.1f));      // [1]
        // 0 by default: no passive drift at all. Negative = slow sink, positive
        // = slow rise (both opt-in; the old -0.02 default crept the player
        // downward whenever no vertical key was held).
        AddSetting(new NumberSetting("Glide", "Passive vertical drift while no vertical key is held", 0.0f, -0.75f, 0.75f, 0.01f)); // [2]
        canBind = true;
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnTick() override;

    float GetHSpeed() const { return static_cast<NumberSetting*>(mSettings[0])->mValue; }
    float GetVSpeed() const { return static_cast<NumberSetting*>(mSettings[1])->mValue; }
    float GetGlide() const { return static_cast<NumberSetting*>(mSettings[2])->mValue; }

private:
    // The AABB we flattened, so disable can restore exactly what we broke.
    // 1.8f is the vanilla player height and what the game itself rebuilds.
    float mSavedHeight = 1.8f;
    bool mCollapsed = false;
};
