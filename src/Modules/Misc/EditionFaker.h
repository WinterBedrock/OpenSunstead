#pragma once
#include "../Module.h"
#include "../../SDK/Client/DeviceEnums.h"

// EditionFaker (1.26.42.01)
//
// DeviceOS / DefaultInputMode are read by the ConnectionRequest JSON builders
// from the AppPlatform singleton via CFG-guarded virtuals. EditionFaker
// replaces each 6-byte `call __guard_dispatch_icall_fptr` site with
// `mov eax, <spoofed>` + NOP; the following `mov edx, eax` forwards the value
// into the game's own JSON field writer. Four sites are covered: the modern
// full toJson builder and the legacy short builder (see EditionFaker.cpp).
class EditionFaker : public Module {
public:
    EditionFaker() : Module("EditionFaker", "Spoofs your device OS and Input Method", Category::Misc, 0) {
        AddSetting(new EnumSetting("OS", "Spoofed operating system",
            static_cast<int>(SDK::DeviceOS::WIN10), SDK::DeviceOSNames()));
        AddSetting(new EnumSetting("Input Method", "Spoofed input method",
            static_cast<int>(SDK::InputMode::MOUSE), SDK::InputModeNames()));
        AddSetting(new EnumSetting("Device Model", "Spoofed Device Model", 0, {
            "None", "PlayStation 5", "Xbox Series X", "Nintendo Switch", "iPad Pro", "iPhone 15 Pro", "Samsung Galaxy S24"
        }));
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnTick() override;

private:
    void ApplySpoof();
    void RestoreOriginals();
};
