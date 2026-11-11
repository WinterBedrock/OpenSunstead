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

    // Custom Angle: current effective angle in degrees (vanilla -20), applied
    // through the direct displacement patch in Swing.cpp.
    float CustomAngleDegrees();

    // Restore the patched instruction to its original displacement. Call before
    // handing the process back (eject path) — the slot itself stays alive.
    static void RestoreAnglePatch();

private:
    static bool InstallHook();

    int   Style() const;
    float Speed() const;
    bool  CustomAngle() const;
    float Angle() const;
};
