#pragma once
#include "../Module.h"

// Swing!
class Swing : public Module {
public:
    Swing();

    void OnEnable() override;
    void OnDisable() override;
    void OnTick() override;
    void UpdateSettingsVisibility() override;

    static void InstallHooks();

    static uintptr_t SwingRotateReturn();
    static uintptr_t SwingRotateFn();
    static float SwingAngleScale();

private:
    static bool InstallHook();

    int   Style() const;
    float Speed() const;
    bool  CustomAngle() const;
    float Angle() const;
};
