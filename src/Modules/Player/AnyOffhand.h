#pragma once
#include "../Module.h"

// Allow any item in the offhand slot (not only totem/arrows/maps/fireworks).
// Hooks Item allow_off_hand validators (Item+0x152 bit7) when enabled.
class AnyOffhand : public Module {
public:
    AnyOffhand();
    void OnEnable() override;
    void OnDisable() override;

    // MinHook targets (called from GameHooks::Initialize once).
    static void InstallHooks();
    static bool IsActive();

private:
    static bool s_hooksInstalled;
};
