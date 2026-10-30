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

// Wire layout, verified against the 1.26.50.04 build. createPacket(9) ->
// sub_1416B1D20 allocates 264 bytes and returns an all-empty body with
// discriminator 0; the real chat body is built by
// ClientInstanceScreenModel::sendChatMessage
// (sub_141885B70 -> sub_143559070 -> sub_143559680) — field offsets are the
// struct's below. The +0xE8 discriminator selects how +0xA8/+0xC8 are read:
//   disc 0 — "message only":   +0xA8 is the message
//   disc 1 — "author+message": +0xA8 = author, message at +0xC8  <- what chat sends
//   disc 2 — "message+params": +0xA8 = message, params at +0xC8
// CAUTION: base ctor sub_143555560 initialises the packet from a temporary
// "formatter" object — its src offsets are the temporary's, NOT the packet's.
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
