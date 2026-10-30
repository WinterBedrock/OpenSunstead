#pragma once
// SDK client-bound packets Sunstead actively uses / injects: TextPacket (0x09,
// chat send) and PlayerAuthInput (0x90, silent rotation inject). Go through
// the helpers here instead of hard-coding packet IDs / field offsets.

#include "Packet.h"
#include "Packets/TextPacket.h"
#include "PacketUtils.h"
#include "../Client/ClientInstance.h"
#include "../Client/Chat.h"
#include <cmath>

namespace SDK {
namespace ClientPackets {

// Send a chat message as the local player. Prefer this over ad-hoc createPacket.
inline bool sendText(const std::string& message) {
    return Chat::sendMessage(message);
}

// Local-only client message (not sent to server).
inline bool displayText(const std::string& message) {
    return Chat::displayClientMessage(message);
}

// Full class: SDK/Network/Packets/PlayerAuthInputPacket.h
// Modules patch live packets in OnPacketSend (e.g. NoFall).

inline constexpr int kPlayerAuthInputId = static_cast<int>(PacketID::PlayerAuthInput);
inline constexpr uintptr_t kPAI_pitch    = 0x30;
inline constexpr uintptr_t kPAI_yaw      = 0x34;
inline constexpr uintptr_t kPAI_yHeadRot = 0x44; // was wrong as 0x38 (that's pos.x)

inline bool isPlayerAuthInput(void* packet) {
    if (!packet || !sdk_safe_ptr(packet, 0x40)) return false;
    __try {
        // Virtual getId via first vtable slot after dtor is unreliable across
        // builds; GameHooks compares against PacketID enum after sehGetPacketId.
        // Here we only expose field writers for a packet already identified.
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Write yaw/pitch into a PlayerAuthInput packet body (pitch=x, yaw=y).
inline bool writeAuthInputLook(void* packet, float yaw, float pitch) {
    if (!packet || !sdk_safe_ptr(packet, kPAI_yHeadRot + 4)) return false;
    if (!std::isfinite(yaw) || !std::isfinite(pitch)) return false;
    if (pitch >  89.9f) pitch =  89.9f;
    if (pitch < -89.9f) pitch = -89.9f;
    __try {
        *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(packet) + kPAI_pitch)    = pitch;
        *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(packet) + kPAI_yaw)      = yaw;
        *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(packet) + kPAI_yHeadRot) = yaw;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline bool readAuthInputLook(void* packet, float& yaw, float& pitch) {
    if (!packet || !sdk_safe_ptr(packet, kPAI_yaw + 4)) return false;
    __try {
        pitch = *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(packet) + kPAI_pitch);
        yaw   = *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(packet) + kPAI_yaw);
        return std::isfinite(yaw) && std::isfinite(pitch);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Generic send helper for any already-built Packet*.
inline bool send(Packet* pkt) {
    return PacketUtils::sendToServer(pkt);
}

} // namespace ClientPackets
} // namespace SDK
