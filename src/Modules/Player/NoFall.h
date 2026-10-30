#pragma once
#include "../Module.h"

class NoFall : public Module {
public:
    enum Mode : int {
        Sentinel = 0,
        BDS      = 1,
        Lifeboat = 2,
        OnGround = 3,
    };

    NoFall();
    int GetMode() const;
    void OnEnable() override;
    void OnDisable() override;
    void OnPacketSend(class SDK::Packet* packet, int packetId, bool& cancel) override;

private:
    bool  mAltToggle = false;
    float mLastY     = 0.f;
};
