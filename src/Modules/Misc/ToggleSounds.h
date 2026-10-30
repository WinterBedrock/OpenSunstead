#pragma once
#include "../Module.h"

class ToggleSounds : public Module {
public:
    ToggleSounds() : Module("ToggleSounds", "Plays a sound when modules are toggled", Category::Misc, 0) {
        // Sound pack selection. Asset .wav files for each pack are bundled in
        // the DLL (RCDATA) and deployed to %APPDATA%/Sunstead/assets on inject.
        AddSetting(new EnumSetting("Type", "Toggle sound pack", 0, { "Default", "Modern", "Smooth" }));
        sInstance = this;
    }

    // Called centrally (ModuleManager / ClickGUI) whenever any module is toggled.
    // Plays the selected sound pack's on/off asset, extracting on demand.
    static void PlayToggleSound(bool enabled);

    // Currently selected pack index (0=Default,1=Modern,2=Smooth), read from
    // the live module instance's setting so the static player can use it.
    static int GetSoundType();

private:
    static ToggleSounds* sInstance;
};
