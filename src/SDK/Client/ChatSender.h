#pragma once
// Screen-independent chat sender: builds TextPacket / CommandRequestPacket
// directly and dispatches them through the client's PacketSender (works from
// anywhere, no chat screen required). Layout/offsets ported from the Latite
// reference SDK as-is.

#include "../Types.h"
#include "../Network/Packets/TextPacket.h"
#include <Windows.h>
#include <string>
#include <memory>
#include <cstdint>

extern uintptr_t g_MinecraftPackets_createPacket;
extern uintptr_t g_TextPacket_buildChat;

namespace SDK {

class ClientInstance; // fwd (defined in ClientInstance.h)

namespace chat {

    enum class BPacketID : int { TEXT = 0x9, COMMAND_REQUEST = 77 };

    // Bedrock Packet base, IDA-verified on 1.26.50.04 (TextPacket ctor
    // 0x143555560): the ctor initializes the fields below and the header ends
    // at 0x30, so every packet body starts at +0x30. (Reserving only 0x28 —
    // the old Latite layout — shifts every body field by 8.)
    class BPacket {
    public:
        virtual ~BPacket() {};
        virtual BPacketID getID() { return BPacketID::TEXT; };
        virtual std::string getName() { return ""; };
        virtual void write(void* stream) {};
        virtual void readExtended(void* stream) {};
        virtual bool allowBatch() { return false; };
        virtual void _read(void* stream) {};
        // IDA-verified header, total size 0x30.
        char pad08[4]{};
        int32_t priority = 2;              // +0x08
        int32_t reliability = 1;           // +0x0C
        uint8_t subClientId = 0;           // +0x10
        bool isHandled = false;            // +0x11
        char pad12[6]{};
        void* unknown = nullptr;           // +0x18
        void*** handler = nullptr;         // +0x20
        int32_t compressibility = 0;       // +0x28
        char pad2C[4]{};
    };

    // Vtable order (declaration order = slot order):
    //   [0] dtor  [1] isInitialized  [2] send  [3] sendTo  [4] sendToServer
    // IDA 1.26.50.04: the game's chat path (ClientInstanceScreenModel::
    // sendChatMessage, sub_141885B70) resolves the sender via CI vt[293]
    // (= CI+0x1D0, what getPacketSender reads) and dispatches `send` at
    // slot [2]. Slot [4] (sendToServer) has an unverified ABI here.
    class BPacketSender {
    public:
        virtual ~BPacketSender() = 0;
        virtual bool isInitialized() = 0;
        virtual void send(BPacket*) = 0;
        virtual void sendTo(void* networkIdentifier, uint8_t subClientId, BPacket* pkt) = 0;
        virtual void sendToServer(BPacket* pkt) = 0;
    };

    // TextPacket::buildChat — sub_143559070 on 1.26.50.04, the exact function
    // the game's chat path uses to construct the chat packet. ABI (from that
    // call site):
    //   rcx packet         TextPacket fresh out of createPacket(9)
    //   rdx author*        std::string (xuid / display name; may be empty)
    //   r8  message*       std::string — the text being sent
    //   r9  filteredFlag*  40-byte struct; byte at +0x20 == 0 means "no
    //                      filtered message", which is what vanilla chat passes
    //   [rsp+0x20] platformId*, [rsp+0x28] platformOnlineId*  (may be empty)
    // Writes type@+0xA0=Chat, author@+0xA8, message@+0xC8, variant@+0xE8=1 —
    // the combination the serializer reads. POD-only body (C2712): the strings
    // are passed by pointer, not stored.
    inline bool sehBuildChat(void* fn, void* packet, std::string* author, std::string* message,
                             void* filteredFlag, std::string* platformId,
                             std::string* platformOnlineId) {
        if (!fn || !packet || !author || !message) return false;
        __try {
            using BuildFn = void(__fastcall*)(void*, std::string*, std::string*,
                                              void*, std::string*, std::string*);
            ((BuildFn)fn)(packet, author, message, filteredFlag, platformId, platformOnlineId);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Build a chat TextPacket and send it to the server. Returns false if the
    // packet/sender/builder could not be resolved. Author/platform fields are
    // left empty; the server fills them from the authenticated connection.
    inline bool sendChat(void* clientInstance, void* packetSender, const std::string& message) {
        if (!clientInstance || !packetSender || !g_MinecraftPackets_createPacket) return false;
        if (!g_TextPacket_buildChat || !sdk_safe_ptr((void*)g_TextPacket_buildChat, 16)) return false;

        using CreateFn = std::shared_ptr<BPacket>(*)(BPacketID);
        std::shared_ptr<BPacket> pkt = ((CreateFn)g_MinecraftPackets_createPacket)(BPacketID::TEXT);
        if (!pkt) return false;

        // Vanilla-equivalent inputs: empty author/platform fields, not-filtered flag.
        std::string author;
        std::string body = message;
        std::string platformId;
        std::string platformOnlineId;
        unsigned char filteredFlag[40] = {0};   // +0x20 == 0 -> no filtered message

        if (!sehBuildChat((void*)g_TextPacket_buildChat, pkt.get(), &author, &body,
                          filteredFlag, &platformId, &platformOnlineId))
            return false;

        // Dispatch via vt[2] (`send`) — the slot the game's chat path uses
        // (see BPacketSender above); [4] is unverified here.
        reinterpret_cast<BPacketSender*>(packetSender)->send(pkt.get());
        return true;
    }

    // CommandRequestPacket (ID 77) — IDA-verified on 1.26.42.01. Factory:
    // MinecraftPackets::createPacket case 77 -> sub_141711C40 -> body ctor
    // sub_14861EA60. Live std::string at +0x30 (SSO buf +0x30, size +0x40,
    // capacity +0x48); CommandOrigin after it: type byte +0x50, mce::UUID
    // +0x58, requestId string +0x68, version +0x88. The factory pre-constructs
    // an empty command + default origin, so writing ONE std::string at +0x30
    // (the command, WITH the leading '/') is a valid packet.
    inline constexpr uintptr_t kCommandStringOffset = 0x30;

    // POD-only SEH body (C2712: __try cannot live beside destructors).
    inline bool sehAssignCommand(void* packet, const char* text) {
        __try {
            std::string& cmd = *reinterpret_cast<std::string*>(
                reinterpret_cast<uintptr_t>(packet) + kCommandStringOffset);
            cmd = text;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        return true;
    }

    // Send a server command ("/help ..."). Returns false if the packet/sender
    // could not be resolved.
    inline bool sendCommand(void* clientInstance, void* packetSender, const std::string& command) {
        if (!clientInstance || !packetSender || !g_MinecraftPackets_createPacket) return false;
        if (command.empty()) return false;

        using CreateFn = std::shared_ptr<BPacket>(*)(BPacketID);
        std::shared_ptr<BPacket> pkt = ((CreateFn)g_MinecraftPackets_createPacket)(BPacketID::COMMAND_REQUEST);
        if (!pkt) return false;

        // A drifted layout would trash the heap; the SEH body bails out instead.
        if (!sehAssignCommand(pkt.get(), command.c_str())) return false;

        // Dispatch via vt[2] (`send`) — the game's chat dispatch slot on this
        // build (see BPacketSender above).
        reinterpret_cast<BPacketSender*>(packetSender)->send(pkt.get());
        return true;
    }

} // namespace chat
} // namespace SDK
