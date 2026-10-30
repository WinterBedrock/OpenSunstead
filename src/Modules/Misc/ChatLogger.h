#pragma once
#include "../Module.h"

namespace SDK { class Packet; }

// ChatLogger — intercepts incoming TextPacket (id 0x09) messages and appends
// clean, human-readable lines to %APPDATA%/Sunstead/chat_log.txt.
//
// It is type-aware: only text the player would actually see in chat is written.
// Translation keys, JSON envelopes and raw XUIDs are decoded or skipped so the
// log never fills with keys like "chat.type.text" or numeric ids.
class ChatLogger : public Module {
public:
    ChatLogger() : Module("ChatLogger", "Logs incoming chat to %APPDATA%/Sunstead/chat_log.txt", Category::Misc, 0) {
        AddSetting(new BoolSetting("Timestamps", "Prefix each line with [HH:MM:SS]", true));            // [0]
        AddSetting(new BoolSetting("System Messages", "Also log system / announcement lines", true));   // [1]
    }

    void OnPacketReceive(class SDK::Packet* packet, int packetId, bool& cancel) override;
};
