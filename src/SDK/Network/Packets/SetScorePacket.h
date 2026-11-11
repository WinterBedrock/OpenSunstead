#pragma once

#include "Packet.h"
#include <cstdint>
#include <string>
#include <vector>

namespace SDK {


// IdentityDefinition::Type — the discriminator byte at ScorePacketInfo+0x58.
namespace IdentityDefinition {
    enum class Type : uint8_t {
        Invalid    = 0,
        Player     = 1,   // +0x38 carries int64 actorUniqueId
        Entity     = 2,   // +0x38 carries int64 actorUniqueId
        FakePlayer = 3,   // +0x38 carries std::string customName
    };
}

#pragma pack(push, 1)
struct ScorePacketInfo {
    int64_t     scoreboardId;                    // +0x00
    int64_t     pad08;                           // +0x08
    std::string objectiveName;                   // +0x10
    int32_t     score;                           // +0x30
    char        pad34[0x4];                      // +0x34
    std::string customName;                      // +0x38 (FakePlayer only)
    uint8_t     identityType;                    // +0x58
    char        pad59[0x7];                      // ends at 0x60 == stride

    IdentityDefinition::Type type() const {
        return static_cast<IdentityDefinition::Type>(identityType);
    }
    bool isFakePlayer() const {
        return type() == IdentityDefinition::Type::FakePlayer;
    }
};
#pragma pack(pop)

static_assert(sizeof(ScorePacketInfo) == 0x60,
              "ScorePacketInfo must be 0x60 (96) bytes (1.26.50/51)");
static_assert(offsetof(ScorePacketInfo, objectiveName) == 0x10, "objectiveName @ 0x10");
static_assert(offsetof(ScorePacketInfo, score)         == 0x30, "score @ 0x30");
static_assert(offsetof(ScorePacketInfo, customName)    == 0x38, "customName @ 0x38");
static_assert(offsetof(ScorePacketInfo, identityType)  == 0x58, "identityType @ 0x58");

class SetScorePacket : public Packet {
public:
    static constexpr PacketID ID = PacketID::SetScore;   // 108

    std::vector<ScorePacketInfo> entries;                // +0x30
    int32_t                    serializationMode = 5;    // +0x48 (cereal-only)
    char                       pad4C[0x4]{};

    size_t count() const { return entries.size(); }

    int replaceFakePlayerName(const std::string& from, const std::string& to) {
        if (from.empty()) return 0;
        int changed = 0;
        for (auto& e : entries) {
            if (!e.isFakePlayer()) continue;
            std::string& name = e.customName;
            size_t pos = 0;
            bool hit = false;
            while ((pos = name.find(from, pos)) != std::string::npos) {
                name.replace(pos, from.size(), to);
                pos += to.size();
                hit = true;
            }
            if (hit) ++changed;
        }
        return changed;
    }
};

static_assert(offsetof(SetScorePacket, entries) == 0x30,
              "SetScorePacket::entries @ 0x30 (1.26.50/51)");
static_assert(offsetof(SetScorePacket, serializationMode) == 0x48,
              "SetScorePacket::serializationMode @ 0x48");

} // namespace SDK
