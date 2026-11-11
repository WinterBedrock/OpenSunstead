#pragma once
#include "../Module.h"

namespace SDK { class Packet; }

class ChatLogger : public Module {
public:
    ChatLogger() : Module("ChatLogger", "Logs incoming chat to %APPDATA%/Sunstead/chat_log.txt", Category::Misc, 0) {
        AddSetting(new BoolSetting("Timestamps", "Prefix each line with [HH:MM:SS]", true));            // [0]
        AddSetting(new BoolSetting("System Messages", "Also log system / announcement lines", true));   // [1]
    }

    void OnPacketReceive(class SDK::Packet* packet, int packetId, bool& cancel) override;
};
