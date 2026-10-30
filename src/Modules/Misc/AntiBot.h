#pragma once
#include "../Module.h"

namespace SDK { class Actor; }

// AntiBot — filters fake/bot entities (Hive "shadow" bots, invalid-hitbox NPCs,
// nametag-only decoys) out of combat targeting. Other combat modules (Aura)
// consult IsBot() before attacking. Mirrors the Solstice AntiBot check set,
// adapted to the Sunstead SDK. Now that the server-side player list works, the
// strongest signal is "player-type actor whose name is NOT in the tab list".
class AntiBot : public Module {
public:
    AntiBot() : Module("AntiBot", "Filters bots out of combat targeting", Category::Misc, 0) {
        AddSetting(new BoolSetting("PlayerList Check", "Treat players missing from the server tab list as bots", true));
        AddSetting(new BoolSetting("Hitbox Check", "Treat players with an abnormal hitbox as bots", true));
        AddSetting(new BoolSetting("Non-Player Check", "Treat non-player actors as bots", true));
        AddSetting(new BoolSetting("Invisible Check", "Treat invisible players as bots", true));
    }

    // True if `actor` should be IGNORED by combat. Always false while the module
    // is disabled, so combat modules can call this unconditionally. Fully
    // SEH-guarded — a recycled/garbage actor can never fault the caller.
    bool IsBot(class SDK::Actor* actor);

private:
    bool GetPlayerListCheck() const { return static_cast<BoolSetting*>(mSettings[0])->mValue; }
    bool GetHitboxCheck()     const { return static_cast<BoolSetting*>(mSettings[1])->mValue; }
    bool GetNonPlayerCheck()  const { return static_cast<BoolSetting*>(mSettings[2])->mValue; }
    bool GetInvisibleCheck()  const { return static_cast<BoolSetting*>(mSettings[3])->mValue; }
};
