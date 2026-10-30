#pragma once
#include "../Module.h"
#include <string>
#include <unordered_set>

// PlayerAlarm — watches the runtime actor list and announces when a player
// enters (green "[+]") or leaves (red "[-]") your render range, via chat and/or
// a client notification. On enable it silently primes the current roster so you
// aren't flooded with a join line for every player already loaded.
namespace SDK { class GuiData; class Actor; class Level; class ClientInstance; }

class PlayerAlarm : public Module {
public:
    enum class Output { Chat = 0, Notification = 1, Both = 2 };
    enum class Event  { Join, Leave };

    PlayerAlarm() : Module("PlayerAlarm", "Alerts when players enter or leave your range", Category::Misc, 0) {
        AddSetting(new EnumSetting("Output", "Where to report join/leave events", 0, { "Chat", "Notification", "Both" })); // [0]
        AddSetting(new BoolSetting("Joins", "Announce players entering range", true));   // [1]
        AddSetting(new BoolSetting("Leaves", "Announce players leaving range", true));   // [2]
    }

    void OnUpdate() override;   // runs on the GAME tick (see ModuleManager::OnUpdate)
    void OnDisable() override;  // clears the tracked roster

private:
    Output GetOutput() const { return static_cast<Output>(static_cast<EnumSetting*>(mSettings[0])->mValue); }
    bool   GetJoins()  const { return static_cast<BoolSetting*>(mSettings[1])->mValue; }
    bool   GetLeaves() const { return static_cast<BoolSetting*>(mSettings[2])->mValue; }

    // Emit one event for `name`, formatted for chat (§-coloured) and/or a
    // notification (plain), per the Output setting.
    void announce(Event ev, const std::string& name);

    std::unordered_set<std::string> mRoster; // player names currently in range
    bool mPrimed = false;                    // false until the first baseline pass
};
