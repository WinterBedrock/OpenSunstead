#pragma once
#include "../Module.h"

class Freelook : public Module {
public:
    Freelook();
    void OnTick() override;
    void OnDisable() override;
    bool IsActive();
private:
    bool mActive  = false; // hold-mode activation state
    bool mPrevKey = false;
};
