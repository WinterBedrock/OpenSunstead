#pragma once
#include "../Module.h"
#include "../../Utils/MCSDK.h"

class PacketLogger : public Module {
public:
    PacketLogger() : Module("PacketLogger", "Logs incoming server packets", Category::Misc, 0) {
        AddSetting(new BoolSetting("Chat", "Print packet summary to chat", true));
        AddSetting(new BoolSetting("Console Dump", "Print packet object bytes to console", true));
        AddSetting(new BoolSetting("Ignore Spam", "Skip very noisy packets", true));
        AddSetting(new NumberSetting("Bytes", "Packet object bytes to dump", 64.0f, 16.0f, 256.0f, 16.0f));
    }

    static void LogIncoming(const std::shared_ptr<SDK::Packet>& packet, int packetId);
};
