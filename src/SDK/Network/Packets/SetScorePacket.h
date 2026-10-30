#pragma once

#include "Packet.h"
#include <cstdint>
#include <string>
#include <vector>

namespace SDK {

// SetScorePacket (SetScore, packet ID 108) — 1.26.50.04 / 1.26.51.01, cereal-only
// (the 1.26.30.05 layout: action@+0x30, entries@+0x38, 104-byte entries is
// STALE — the members moved).
//   base Packet = 0x30; send ctor sub_14055F030; cereal read = vtable[5].
//   +0x30  std::vector<ScorePacketInfo> entries
//   +0x48  int32 serializationMode (5 = CerealOnly)
// ScorePacketInfo — 0x60 / 96 bytes per entry (stride proven by the vector copy
// helper sub_143B48650; FakePlayer entry-copy sig unique on both exes: RVA
// 0x3B49020 / 0x3B4A150):
//   +0x00  i64 scoreboardId        +0x08  i64 pad
//   +0x10  std::string objectiveName (size@+0x20, cap@+0x28)
//   +0x30  i32 score               +0x34  pad[4]
//   +0x38  std::string customName (FakePlayer; size@+0x48, cap@+0x50) —
//          int64 actorUniqueId for Player/Entity entries
//   +0x58  u8 identityType (0=Invalid 1=Player 2=Entity 3=FakePlayer)
// Raw overlay onto the packet's vector entries: do NOT construct or copy —
// the std::string members alias game-owned storage.

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

    // Rewrite every FakePlayer customName substring `from` -> `to`; returns
    // the number of entries modified. NOT SEH-guarded — guard at the call
    // site on live/untrusted packets.
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
