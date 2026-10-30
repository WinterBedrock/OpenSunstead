#pragma once
#include "../Module.h"
#include <imgui.h>
#include <vector>
#include "../../SDK/Network/Packet.h"

class Blink : public Module {
public:
    Blink() : Module("Blink", "Freeze outgoing packets via LoopbackPacketSender::send", Category::Misc, 0) {
        AddSetting(new BoolSetting("Freeze All", "Freeze EVERY outgoing packet type, including network", false));        // [0]
        AddSetting(new BoolSetting("Only Movement", "Freeze only PlayerAuthInput / MovePlayer", true));                  // [1]
        AddSetting(new BoolSetting("Store Queue", "Queue frozen packets to memory (else drop)", true));                  // [2]
        AddSetting(new BoolSetting("Flush On Disable", "Dispatch stored queue to the server when the module is turned off", true)); // [3]
        AddSetting(new NumberSetting("Max Packets/s", "Rate-cap for the flush loop", 60.0f, 1.0f, 500.0f, 1.0f));  // [4]
    }
    void OnTick() override;
    void OnDisable() override;
    void OnPacketSend(class SDK::Packet* packet, int packetId, bool& cancel) override;
    void DrainTick();

    bool ConsumeFrost(SDK::Packet* packet, int id);

private:
    struct Queued {
        int  id;             // pkt id
        unsigned long long time;
    };
    std::vector<Queued> mQueue;
    bool mFrozenAll = false;
    unsigned long long mLastFlushMs = 0;
public:
    bool IsDraining() const { return !mQueue.empty(); }
};
