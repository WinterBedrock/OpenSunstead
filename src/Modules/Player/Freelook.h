#pragma once
#include "../Module.h"

// Free camera. Bind Type selects how the keybind behaves:
//   Toggle (default) - the keybind flips the module on/off, exactly like a
//                      normal module (handled by the global OnKey dispatch).
//   Hold             - active only while the key is physically held (self-poll,
//                      same approach as the Zoom module).
// While active the perspective is forced to third person and the player's body
// (aim/look) is frozen so only the camera orbits freely.
class Freelook : public Module {
public:
    Freelook();
    void OnTick() override;
    // Leaving the module (toggle off OR hold-key release, which force-disables
    // it) must clear mActive: the camera hooks read IsActive(), and OnTick no
    // longer runs to reset it once disabled.
    void OnDisable() override;
    bool IsActive();
private:
    bool mActive  = false; // hold-mode activation state
    bool mPrevKey = false;
};
