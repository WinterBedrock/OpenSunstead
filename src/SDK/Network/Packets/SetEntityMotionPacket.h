#pragma once

#include "Packet.h"
#include "../../Types.h"

namespace SDK {

class SetEntityMotionPacket : public Packet {
public:
    static constexpr PacketID ID = PacketID::SetEntityMotion;

    uint64_t runtimeId; // +0x30
    Vec3 motion;        // +0x38
};

using SetActorMotionPacket = SetEntityMotionPacket;

} // namespace SDK

