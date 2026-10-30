#pragma once
#include "../Module.h"

// KickSounds — plays a sound when a Disconnect packet (id 0x5) is received,
// i.e. when the local player gets kicked. Sound is selectable in settings:
//   0 = Fard, 1 = Mario, 2 = Windows Error.
class KickSounds : public Module {
public:
    KickSounds() : Module("KickSounds", "Plays a sound when you get kicked", Category::Misc, 0) {
        AddSetting(new EnumSetting("Sound", "Which sound to play on kick", 0, { "Fard", "Mario", "Windows Error", "67" }));
    }

    void OnPacketReceive(class SDK::Packet* packet, int packetId, bool& cancel) override;

    int GetSoundIndex() const { return static_cast<EnumSetting*>(mSettings[0])->mValue; }
};
