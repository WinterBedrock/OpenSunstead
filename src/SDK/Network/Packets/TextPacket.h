#pragma once

#include "Packet.h"
#include <string>

namespace SDK {

enum class TextPacketType : unsigned char {
    Raw = 0,
    Chat = 1,
    Translate = 2,
    Popup = 3,
    JukeboxPopup = 4,
    Tip = 5,
    SystemMessage = 6,
    Whisper = 7,
    Announcement = 8,
    TextObjectWhisper = 9,
    TextObject = 10,
    TextObjectAnnouncement = 11,
};

class TextPacket : public Packet {
public:
    static constexpr PacketID ID = PacketID::Text;

    // header 0x00..0x2F, initialised by the base ctor
    char pad00[0x30]{};

    bool translationNeeded = false;        // +0x30
    char pad31[0x7]{};
    std::string platformChatId;            // +0x38
    std::string platformOnlineId;          // +0x58
    std::string filteredMessage;           // +0x78  (only sent when +0x98 == 1)
    bool filteredPresent = false;          // +0x98
    char pad99[0x7]{};
    TextPacketType type = TextPacketType::Raw;  // +0xA0
    char padA1[0x7]{};
    std::string author;                    // +0xA8  (message when disc == 0)
    char padB8[0x10]{};                    // +0xB8  capacity tail of +0xA8
    std::string message;                   // +0xC8  (author's text when disc == 1)
    char padD8[0x10]{};                    // +0xD8  capacity tail of +0xC8
    uint8_t variantIndex = 0;              // +0xE8
    char padE9[0x3]{};
    int32_t unknownF0 = 5;                 // +0xF0
};

} // namespace SDK
