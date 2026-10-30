#pragma once
#include "../Module.h"

class InventoryMove : public Module {
public:
    InventoryMove() : Module("InventoryMove", "Allows you to move while inventory/container is open", Category::Movement, 0) {}

    void OnTick() override;
    void OnPacketReceive(class SDK::Packet* packet, int packetId, bool& cancel) override;
};
