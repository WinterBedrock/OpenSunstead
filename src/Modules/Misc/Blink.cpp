#include "Blink.h"
#include "../../Hooks/GameHooks.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/SDK.h"
#include "../../SDK/Network/Packet.h"
#include "../Setting.h"
#include <imgui.h>
#include <Windows.h>
#include "../../SDK/Network/PacketUtils.h"

static bool blinkIsMovementPacket(int id) {
    return id == (int)SDK::PacketID::PlayerAuthInput || id == (int)SDK::PacketID::MovePlayer;
}

bool Blink::ConsumeFrost(SDK::Packet* packet, int id) {
    if (!IsEnabled()) return false;
    const bool freezeAll = static_cast<BoolSetting*>(mSettings[0])->mValue;
    const bool onlyMove  = static_cast<BoolSetting*>(mSettings[1])->mValue;
    const bool storeQ    = static_cast<BoolSetting*>(mSettings[2])->mValue;

    bool gate = false;
    if (freezeAll) gate = true;
    else if (onlyMove) gate = blinkIsMovementPacket(id);

    if (!gate) return false;
    if (storeQ) {
        Queued q;
        q.id = id;
        q.time = GetTickCount64();
        mQueue.push_back(q);
    }
    return true;   // freeze, never call the game's sender
}

void Blink::OnTick() {
    DrainTick();
}

void Blink::DrainTick() {
    if (IsEnabled() || mQueue.empty()) return;
    bool flushEnabled = static_cast<BoolSetting*>(mSettings[3])->mValue;
    float rateLimit = static_cast<NumberSetting*>(mSettings[4])->mValue;
    if (!flushEnabled) {
        mQueue.clear();
        return;
    }
    unsigned long long now = GetTickCount64();
    unsigned long long minInterval = (unsigned long long)(1000.0f / rateLimit);
    int sent = 0;
    while (!mQueue.empty() && sent < 3) {
        if (minInterval > 0 && (now - mLastFlushMs) < minInterval) break;
        Queued q = mQueue.front();
        mQueue.erase(mQueue.begin());
        std::shared_ptr<SDK::Packet> pkt =
            SDK::MinecraftPackets::createPacket((SDK::PacketID)q.id);
        if (pkt) SDK::PacketUtils::sendToServer(pkt.get());
        mLastFlushMs = now;
        ++sent;
    }
}

void Blink::OnDisable() {
    bool flush = static_cast<BoolSetting*>(mSettings[3])->mValue;
    if (!flush) mQueue.clear();
}

void Blink::OnPacketSend(class SDK::Packet* packet, int packetId, bool& cancel) {
    (void)packet; (void)packetId; (void)cancel;
}
