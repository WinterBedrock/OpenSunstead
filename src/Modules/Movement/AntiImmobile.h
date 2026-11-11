#pragma once
#include "../Module.h"
#include "../Setting.h"
#include <cstdint>

class AntiImmobile : public Module {
public:
    AntiImmobile();

    void OnEnable() override;
    void OnUpdate() override;
    void OnTick() override;

private:
    bool notify() const;
    bool clearTag() const;

    void clearNow(bool allowTagClear);

    uintptr_t mTagAssureFn = 0;  // assure<ActorIsImmobileFlagComponent>, from setImmobile's body
    bool      mReported    = false;
};
