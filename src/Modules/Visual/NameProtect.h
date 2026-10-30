#pragma once
#include "../Module.h"

namespace SDK { class Actor; }

class NameProtect : public Module {
public:
    NameProtect() : Module("NameProtect", "Replaces your name with a custom nick (client-side)", Category::Visual, 0) {
        // [0] the nick name to display
        AddSetting(new StringSetting("Nick", "The fake name to show", "Sunstead User<3"));
        // [1] text color (MCBE formatting codes)
        AddSetting(new EnumSetting("Color", "Text color", 0, {
            "None",
            "White", "Black", "Dark Gray", "Gray",
            "Red", "Dark Red", "Gold", "Yellow",
            "Green", "Dark Green", "Aqua", "Dark Aqua",
            "Blue", "Dark Blue", "Light Purple", "Dark Purple"
        }));
        // [2] bold formatting
        AddSetting(new BoolSetting("Bold", "Bold text", false));
        // [3] obfuscated formatting
        AddSetting(new BoolSetting("Obfuscated", "Obfuscated text", false));
        // [4] italic formatting
        AddSetting(new BoolSetting("Italic", "Italic text", false));
        // [5] also replace the real name on server scoreboards (SetScore packet)
        AddSetting(new BoolSetting("Scoreboard", "Replace your name on scoreboards", true));
        // [6] also replace the real name in chat messages (TextPacket)
        AddSetting(new BoolSetting("Messages", "Replace your name in chat messages", true));
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnTick() override;
    void OnPacketReceive(class SDK::Packet* packet, int packetId, bool& cancel) override;

private:
    std::string mOriginalName;        // saved real name, restored on disable
    bool mHasOriginal = false;        // whether we've captured the original name

    // Builds the formatted nick string with MCBE formatting codes
    std::string BuildFormattedNick() const;
    // Strips MCBE formatting codes (§X) from a string
    static std::string StripColorCodes(const std::string& s);
    // Replaces the real name with the nick inside a SetScore packet's
    // fake-player entries. SEH-guarded; safe on malformed packets.
    void ReplaceScoreboardNames(class SDK::Packet* packet);
    // Rewrites the real name to the nick everywhere it appears in a TextPacket
    // (inline string fields AND the params vector for translate-type chat).
    void RewriteTextPacketNames(class SDK::Packet* packet, const std::string& nick);
};

