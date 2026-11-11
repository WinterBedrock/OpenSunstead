#pragma once

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

    class BPacketSender {
    public:
        virtual ~BPacketSender() = 0;
        virtual bool isInitialized() = 0;
        virtual void send(BPacket*) = 0;
        virtual void sendTo(void* networkIdentifier, uint8_t subClientId, BPacket* pkt) = 0;
        virtual void sendToServer(BPacket* pkt) = 0;
    };

    // Game return object, not a shared_ptr: its deleting destructor passes 0xF8
    // to operator delete at 0x140300054. The game ctor initializes these bytes.
    struct alignas(8) BuiltChatStorage {
        unsigned char bytes[0xF8];
    };
    static_assert(sizeof(BuiltChatStorage) == 0xF8, "TextPacket return size");

    // ABI shape read by 0x140110E50: MSVC string, then engaged flag at +0x20.
    struct FilteredChatMessage {
        std::string value;
        bool engaged = false;
    };
    static_assert(sizeof(FilteredChatMessage) == 0x28, "filtered-message ABI");

    inline bool sehBuildChat(void* fn, BuiltChatStorage* result, std::string* author,
                             std::string* message, FilteredChatMessage* filteredMessage,
                             std::string* platformId, std::string* platformOnlineId) {
        if (!fn || !result || !author || !message || !filteredMessage ||
            !platformId || !platformOnlineId) return false;
        __try {
            // RCX is the hidden result pointer. RDX/R8 are author/message;
            // R9 is the filtered string container; the other two use the stack.
            using BuildFn = BuiltChatStorage*(__fastcall*)(
                BuiltChatStorage*, std::string*, std::string*, FilteredChatMessage*,
                std::string*, std::string*);
            return reinterpret_cast<BuildFn>(fn)(result, author, message, filteredMessage,
                                                 platformId, platformOnlineId) == result;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    inline bool sehSendBuiltChat(void* packetSender, BuiltChatStorage* result) {
        if (!packetSender || !result) return false;
        __try {
            reinterpret_cast<BPacketSender*>(packetSender)->send(
                reinterpret_cast<BPacket*>(result));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    inline bool sehDestroyBuiltChat(BuiltChatStorage* result) {
        if (!result) return false;
        __try {
            // vftable[0] is the game's scalar-deleting destructor (0x140300030).
            // Flag zero destroys owned strings without deleting this stack storage.
            using DestroyFn = BuiltChatStorage*(__fastcall*)(BuiltChatStorage*, unsigned int);
            void** vtable = *reinterpret_cast<void***>(result);
            if (!vtable) return false;
            reinterpret_cast<DestroyFn>(vtable[0])(result, 0);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    inline bool sendChat(void* clientInstance, void* packetSender, const std::string& message) {
        if (!clientInstance || !packetSender) return false;
        if (!g_TextPacket_buildChat || !sdk_safe_ptr((void*)g_TextPacket_buildChat, 16)) return false;

        // Empty author/platform fields and an absent filtered-message payload.
        std::string author;
        std::string body = message;
        std::string platformId;
        std::string platformOnlineId;
        FilteredChatMessage filteredMessage{};
        BuiltChatStorage result{};

        if (!sehBuildChat((void*)g_TextPacket_buildChat, &result, &author, &body,
                          &filteredMessage, &platformId, &platformOnlineId))
            return false;

        // The sender borrows the object only for this call. Destroy it afterwards
        // exactly as the game's stack-allocated TextPacket callers do.
        const bool sent = sehSendBuiltChat(packetSender, &result);
        const bool destroyed = sehDestroyBuiltChat(&result);
        return sent && destroyed;
    }

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
