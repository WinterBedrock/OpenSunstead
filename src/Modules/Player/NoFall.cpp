#include "NoFall.h"
#include "../Setting.h"
#include "../../SDK/Network/Packet.h"
#include "../../SDK/Network/Packets/PlayerAuthInputPacket.h"
#include "../../SDK/SDK.h"
#include "../../Core/Logger.h"

static constexpr uintptr_t kMP_mOnGround = 0x51;

NoFall::NoFall()
    : Module("NoFall", "Prevents fall damage", Category::Player, 0) {
    AddSetting(new EnumSetting("Mode", "NoFall method (Boost parity)", Mode::Sentinel, {
        "Sentinel",
        "BDS",
        "Lifeboat",
        "OnGround",
    }));
    canBind = true;
}

int NoFall::GetMode() const {
    return static_cast<const EnumSetting*>(mSettings[0])->mValue;
}

void NoFall::OnEnable() {
    mAltToggle = false;
    mLastY = 0.f;
}

void NoFall::OnDisable() {
    mAltToggle = false;
    mLastY = 0.f;
}

void NoFall::OnPacketSend(SDK::Packet* packet, int packetId, bool& /*cancel*/) {
    if (!IsEnabled() || !packet) return;

    const int mode = GetMode();

    if (packetId == static_cast<int>(SDK::PacketID::PlayerAuthInput)) {
        if (!sdk_safe_ptr(packet, 0xA0)) return;
        auto* pai = reinterpret_cast<SDK::PlayerAuthInputPacket*>(packet);

        if (mode == Mode::Sentinel) {
            // Boost: alternate ticks; every other packet sets grounded posDelta.y
            mAltToggle = !mAltToggle;
            if (mAltToggle) return;
            pai->posDelta().y = SDK::PlayerAuthInputPacket::kGroundPosDeltaY;
        } else if (mode == Mode::BDS) {
            pai->addInput(SDK::AuthInputAction::START_GLIDING);
            pai->removeInput(SDK::AuthInputAction::STOP_GLIDING);
            pai->posDelta() = Vec3{0.f, 0.f, 0.f};
        } else if (mode == Mode::Lifeboat) {
            mAltToggle = !mAltToggle;
            if (mAltToggle) {
                mLastY = pai->pos().y;
            } else {
                pai->pos().y = mLastY + 0.1f;
                pai->posDelta() = Vec3{0.f, 0.f, 0.f};
                pai->removeMovingInput();
            }
        } else if (mode == Mode::OnGround) {
            // PAI-only servers: vertical collision + grounded delta
            pai->spoofOnGround();
        }
        return;
    }

    // MovePlayer onGround spoof (rare on modern Bedrock, still used by some)
    if (packetId == static_cast<int>(SDK::PacketID::MovePlayer) && mode == Mode::OnGround) {
        if (!sdk_safe_ptr(packet, kMP_mOnGround + 1)) return;
        *reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(packet) + kMP_mOnGround) = true;
    }
}
