#pragma once
#include "../Module.h"

namespace SDK { class Packet; }

class AntiEffect : public Module {
public:
    AntiEffect() : Module("AntiEffect", "Cancels selected effects client-side", Category::Player, 0) {
        AddSetting(new EnumSetting("Mode", "How effects are cancelled", 1,
            { "Purge", "Block All", "Both" }));                                                       // [0]
        AddSetting(new BoolSetting("Levitation", "", true));                                          // [1]
        AddSetting(new BoolSetting("Darkness", "", true));                                            // [2]
        AddSetting(new BoolSetting("Blindness", "", true));                                           // [3]
        AddSetting(new BoolSetting("Nausea", "", true));                                              // [4]
        AddSetting(new BoolSetting("Poison (incl. Fatal)", "", true));                                // [5]
        AddSetting(new BoolSetting("Wither", "", true));                                              // [6]
        AddSetting(new BoolSetting("Hunger", "", false));                                             // [7]
        AddSetting(new BoolSetting("Slowness", "", false));                                           // [8]
        AddSetting(new BoolSetting("Weakness", "", false));                                           // [9]
        AddSetting(new BoolSetting("Mining Fatigue", "", false));                                     // [10]
    }

    void OnUpdate() override;
    void OnPacketReceive(SDK::Packet* packet, int packetId, bool& cancel) override;

    int  GetMode() const { return static_cast<EnumSetting*>(mSettings[0])->mValue; }
    bool GetLevitation()    const { return static_cast<BoolSetting*>(mSettings[1])->mValue; }
    bool GetDarkness()      const { return static_cast<BoolSetting*>(mSettings[2])->mValue; }
    bool GetBlindness()     const { return static_cast<BoolSetting*>(mSettings[3])->mValue; }
    bool GetNausea()        const { return static_cast<BoolSetting*>(mSettings[4])->mValue; }
    bool GetPoison()        const { return static_cast<BoolSetting*>(mSettings[5])->mValue; }
    bool GetWither()        const { return static_cast<BoolSetting*>(mSettings[6])->mValue; }
    bool GetHunger()        const { return static_cast<BoolSetting*>(mSettings[7])->mValue; }
    bool GetSlowness()      const { return static_cast<BoolSetting*>(mSettings[8])->mValue; }
    bool GetWeakness()      const { return static_cast<BoolSetting*>(mSettings[9])->mValue; }
    bool GetMiningFatigue() const { return static_cast<BoolSetting*>(mSettings[10])->mValue; }

private:
    bool isBlocked(int id) const;
};
