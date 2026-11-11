#pragma once
#include "../Module.h"

class PlaceDelay : public Module {
public:
    PlaceDelay();

    void OnEnable() override;
    void OnDisable() override;
    void OnTick() override;

    bool GetOnlyScaffold() const { return static_cast<BoolSetting*>(mSettings[0])->mValue; }
    float GetClickDelayMs() const { return static_cast<NumberSetting*>(mSettings[1])->mValue; }

private:
    void* mPatchedGm = nullptr;   // GameMode whose gm+0xA0 we forced (restore target)
    unsigned long long mLastClickMs = 0;
};
