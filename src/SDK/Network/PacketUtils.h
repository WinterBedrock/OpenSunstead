#pragma once

#include "../Types.h"
#include "Packet.h"
#include "../Client/ClientInstance.h"
#include "../Actor/Actor.h"
#include "../Inventory/Inventory.h"
#include <memory>

namespace SDK {

class BedrockPacketSender {
public:
    virtual ~BedrockPacketSender() = 0;
    virtual bool isInitialized() = 0;
    virtual void send(Packet*) = 0;
    virtual void sendTo(void* networkIdentifier, uint8_t subClientId, Packet* pkt) = 0;
    virtual void sendToServer(Packet* pkt) = 0;
};

namespace PacketUtils {
    inline void fillMobEquipment(Packet* pkt, uint64_t runtimeId, int slot) {
        if (!pkt) return;
        uintptr_t base = (uintptr_t)pkt;
        if (!sdk_safe_ptr(pkt, 0xA4)) return;
        __try {
            *(uint64_t*)(base + 0x30) = runtimeId;   // mRuntimeId
            *(int*)     (base + 0x98) = slot;         // mSlot
            *(int*)     (base + 0x9C) = slot;         // mSelectedSlot
            *(uint8_t*) (base + 0xA0) = 0;            // mContainerId (inventory)
            *(uint8_t*) (base + 0xA1) = (uint8_t)slot;// mSlotByte
            *(uint8_t*) (base + 0xA2) = (uint8_t)slot;// mSelectedSlotByte
            *(uint8_t*) (base + 0xA3) = 0;            // mContainerIdByte
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    inline bool sendToServer(Packet* pkt) {
        if (!pkt) return false;
        ClientInstance* ci = ClientInstance::get();
        if (!ci || !sdk_safe_ptr(ci, sizeof(void*))) return false;
        PacketSender* ps = ci->getPacketSender();
        if (!ps || !sdk_safe_ptr(ps, sizeof(void*))) return false;
        void** vt = *reinterpret_cast<void***>(ps);
        if (!vt || !sdk_safe_ptr(vt, sizeof(void*) * 3)) return false;
        using SendFn = void(__fastcall*)(void*, Packet*);
        SendFn fn = reinterpret_cast<SendFn>(vt[2]);   // PacketSender::send
        if (!fn || !sdk_safe_ptr((void*)fn, 16)) return false;
        __try {
            fn(ps, pkt);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    inline uint64_t writeSlotAndGetRuntimeId(Actor* lp, int slot) {
        uint64_t runtimeId = 0;
        __try {
            SDK::PlayerInventory* inv = lp->getSupplies();
            if (inv) inv->setSelectedSlot(slot);
            runtimeId = lp->getRuntimeID();
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return runtimeId;
    }

    inline bool sendHotbarSlot(Actor* lp, int slot) {
        if (!lp || slot < 0 || slot > 8) return false;

        uint64_t runtimeId = writeSlotAndGetRuntimeId(lp, slot);

        if (!g_MinecraftPackets_createPacket) return false;
        std::shared_ptr<Packet> pkt =
            MinecraftPackets::createPacket((PacketID)31 /*MobEquipment*/);
        if (!pkt) return false;

        fillMobEquipment(pkt.get(), runtimeId, slot);
        return sendToServer(pkt.get());
    }

    inline uint64_t sehRuntimeIdPOD(Actor* lp) {
        if (!lp) return 0;
        __try { return lp->getRuntimeID(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    inline bool sendMobEquipment(Actor* lp, int slot) {
        if (!lp || slot < 0 || slot > 8 || !g_MinecraftPackets_createPacket) return false;
        uint64_t runtimeId = sehRuntimeIdPOD(lp);
        if (!runtimeId) return false;

        std::shared_ptr<Packet> pkt =
            MinecraftPackets::createPacket((PacketID)31 /*MobEquipment*/);
        if (!pkt) return false;

        fillMobEquipment(pkt.get(), runtimeId, slot);
        return sendToServer(pkt.get());
    }

    inline bool fillAnimateSwing(Packet* pkt, uint64_t runtimeId) {
        if (!pkt || !sdk_safe_ptr(pkt, 0x50)) return false;
        __try {
            *(uint64_t*)((uintptr_t)pkt + 0x30) = runtimeId;
            *(uint32_t*)((uintptr_t)pkt + 0x38) = 1;   // Swing
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        return true;
    }

    inline bool sendSwing(Actor* lp) {
        if (!lp || !g_MinecraftPackets_createPacket) return false;
        uint64_t runtimeId = sehRuntimeIdPOD(lp);
        if (!runtimeId) return false;

        std::shared_ptr<Packet> pkt =
            MinecraftPackets::createPacket((PacketID)44 /*Animate*/);
        if (!pkt) return false;

        if (!fillAnimateSwing(pkt.get(), runtimeId)) return false;
        return sendToServer(pkt.get());
    }

} // namespace PacketUtils
} // namespace SDK
