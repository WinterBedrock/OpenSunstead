#pragma once
// PlayerAuthInputPacket (ID 0x90) — Bedrock 1.26.x, Packet base = 0x30.
// Offsets re-verified against the 26.60 payload validator:
// PlayerAuthInputPacketPayload::validate reads mRot+0x30..mInputData+0x88 at
// exactly these offsets (see Validate::* strings).

#include "../Packet.h"
#include "../../Types.h"
#include <cstdint>

namespace SDK {

enum class AuthInputAction : uint64_t {
    NONE = 0,
    ASCEND = 1ULL << 0,
    DESCEND = 1ULL << 1,
    NORTH_JUMP_DEPRECATED = 1ULL << 2,
    JUMP_DOWN = 1ULL << 3,
    SPRINT_DOWN = 1ULL << 4,
    CHANGE_HEIGHT = 1ULL << 5,
    JUMPING = 1ULL << 6,
    AUTO_JUMPING_IN_WATER = 1ULL << 7,
    SNEAKING = 1ULL << 8,
    SNEAK_DOWN = 1ULL << 9,
    UP = 1ULL << 10,
    DOWN = 1ULL << 11,
    LEFT = 1ULL << 12,
    RIGHT = 1ULL << 13,
    UP_LEFT = 1ULL << 14,
    UP_RIGHT = 1ULL << 15,
    WANT_UP = 1ULL << 16,
    WANT_DOWN = 1ULL << 17,
    WANT_DOWN_SLOW = 1ULL << 18,
    WANT_UP_SLOW = 1ULL << 19,
    SPRINTING = 1ULL << 20,
    ASCEND_SCAFFOLDING = 1ULL << 21,
    DESCEND_SCAFFOLDING = 1ULL << 22,
    SNEAK_TOGGLE_DOWN = 1ULL << 23,
    PERSIST_SNEAK = 1ULL << 24,
    START_SPRINTING = 1ULL << 25,
    STOP_SPRINTING = 1ULL << 26,
    START_SNEAKING = 1ULL << 27,
    STOP_SNEAKING = 1ULL << 28,
    START_SWIMMING = 1ULL << 29,
    STOP_SWIMMING = 1ULL << 30,
    START_JUMPING = 1ULL << 31,
    START_GLIDING = 1ULL << 32,
    STOP_GLIDING = 1ULL << 33,
    PERFORM_ITEM_INTERACTION = 1ULL << 34,
    PERFORM_BLOCK_ACTIONS = 1ULL << 35,
    PERFORM_ITEM_STACK_REQUEST = 1ULL << 36,
    HANDLED_TELEPORT = 1ULL << 37,
    EMOTING = 1ULL << 38,
    MISSED_SWING = 1ULL << 39,
    START_CRAWLING = 1ULL << 40,
    STOP_CRAWLING = 1ULL << 41,
    START_FLYING = 1ULL << 42,
    STOP_FLYING = 1ULL << 43,
    RECEIVED_SERVER_DATA = 1ULL << 44,
    IN_CLIENT_PREDICTED_IN_VEHICLE = 1ULL << 45,
    PADDLE_LEFT = 1ULL << 46,
    PADDLE_RIGHT = 1ULL << 47,
    BLOCK_BREAKING_DELAY_ENABLED = 1ULL << 48,
    HORIZONTAL_COLLISION = 1ULL << 49,
    VERTICAL_COLLISION = 1ULL << 50,
    DOWN_LEFT = 1ULL << 51,
    DOWN_RIGHT = 1ULL << 52,
    START_USING_ITEM = 1ULL << 53,
};

inline AuthInputAction operator|(AuthInputAction a, AuthInputAction b) {
    return static_cast<AuthInputAction>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
}
inline AuthInputAction operator&(AuthInputAction a, AuthInputAction b) {
    return static_cast<AuthInputAction>(static_cast<uint64_t>(a) & static_cast<uint64_t>(b));
}
inline AuthInputAction operator~(AuthInputAction a) {
    return static_cast<AuthInputAction>(~static_cast<uint64_t>(a));
}
inline void operator|=(AuthInputAction& a, AuthInputAction b) { a = a | b; }
inline void operator&=(AuthInputAction& a, AuthInputAction b) { a = a & b; }

// Live packet view — do not construct; cast from the send-hook pointer only.
// Layout (Packet base 0x30), offsets mirrored in the k* constants below:
// mRot{pitch,yaw}@0x30, mPos@0x38, mYHeadRot@0x44, mPosDelta@0x48,
// mVehicleRot@0x54, mAnalogMove@0x5C, mMove@0x64, mInteractRots{pitch,yaw}@0x6C,
// mCamOrient@0x74, mRawMove@0x80, mInputData(u64)@0x88, input/play modes
// @0x90/0x94, mInteractModel@0x98, mClientTick@0xA0.
class PlayerAuthInputPacket : public Packet {
public:
    static constexpr PacketID ID = PacketID::PlayerAuthInput;

    static constexpr uintptr_t kRot            = 0x30;
    static constexpr uintptr_t kPos            = 0x38;
    static constexpr uintptr_t kYHeadRot       = 0x44;
    static constexpr uintptr_t kPosDelta       = 0x48;
    static constexpr uintptr_t kVehicleRot     = 0x54;
    static constexpr uintptr_t kAnalogMove     = 0x5C;
    static constexpr uintptr_t kMove           = 0x64;
    static constexpr uintptr_t kInteractRots   = 0x6C;
    static constexpr uintptr_t kCamOrient      = 0x74;
    static constexpr uintptr_t kRawMove        = 0x80;
    static constexpr uintptr_t kInputData      = 0x88;
    static constexpr uintptr_t kInputMode      = 0x90;
    static constexpr uintptr_t kPlayMode       = 0x94;
    static constexpr uintptr_t kInteractModel  = 0x98;
    static constexpr uintptr_t kClientTick     = 0xA0;

    static constexpr float kGroundPosDeltaY = -0.0784000015258789f;

    float& pitch()    { return *reinterpret_cast<float*>(base() + kRot); }
    float& yaw()      { return *reinterpret_cast<float*>(base() + kRot + 4); }
    float& yHeadRot() { return *reinterpret_cast<float*>(base() + kYHeadRot); }

    Vec3& pos()       { return *reinterpret_cast<Vec3*>(base() + kPos); }
    Vec3& posDelta()  { return *reinterpret_cast<Vec3*>(base() + kPosDelta); }

    // Move vectors the server's movement predictor integrates (all three must
    // stay mutually consistent — rewrite all or none).
    Vec2& moveVec()      { return *reinterpret_cast<Vec2*>(base() + kMove); }
    Vec2& analogMoveVec(){ return *reinterpret_cast<Vec2*>(base() + kAnalogMove); }
    Vec2& rawMoveVec()   { return *reinterpret_cast<Vec2*>(base() + kRawMove); }

    uint64_t& inputDataRaw() { return *reinterpret_cast<uint64_t*>(base() + kInputData); }
    AuthInputAction inputData() const {
        return static_cast<AuthInputAction>(*reinterpret_cast<const uint64_t*>(base() + kInputData));
    }
    void setInputData(AuthInputAction a) {
        *reinterpret_cast<uint64_t*>(base() + kInputData) = static_cast<uint64_t>(a);
    }
    void addInput(AuthInputAction a) {
        inputDataRaw() |= static_cast<uint64_t>(a);
    }
    void removeInput(AuthInputAction a) {
        inputDataRaw() &= ~static_cast<uint64_t>(a);
    }
    bool hasInput(AuthInputAction a) const {
        return (static_cast<uint64_t>(inputData()) & static_cast<uint64_t>(a)) != 0;
    }

    int& inputMode()  { return *reinterpret_cast<int*>(base() + kInputMode); }
    int64_t& clientTick() { return *reinterpret_cast<int64_t*>(base() + kClientTick); }

    void setLook(float y, float p) {
        pitch() = p;
        yaw() = y;
        yHeadRot() = y;
        *reinterpret_cast<float*>(base() + kInteractRots) = p;
        *reinterpret_cast<float*>(base() + kInteractRots + 4) = y;
    }

    // NoFall OnGround: spoof "standing on ground" for PAI-only servers.
    // Bedrock PAI has no bool onGround — servers use vertical collision + posDelta.y.
    void spoofOnGround() {
        Vec3& d = posDelta();
        d.y = kGroundPosDeltaY;
        addInput(AuthInputAction::VERTICAL_COLLISION);
        removeInput(AuthInputAction::JUMPING);
        removeInput(AuthInputAction::START_JUMPING);
        removeInput(AuthInputAction::START_GLIDING);
    }

    void removeMovingInput() {
        removeInput(AuthInputAction::UP);
        removeInput(AuthInputAction::DOWN);
        removeInput(AuthInputAction::LEFT);
        removeInput(AuthInputAction::RIGHT);
        removeInput(AuthInputAction::UP_LEFT);
        removeInput(AuthInputAction::UP_RIGHT);
        removeInput(AuthInputAction::DOWN_LEFT);
        removeInput(AuthInputAction::DOWN_RIGHT);
        *reinterpret_cast<Vec2*>(base() + kAnalogMove) = Vec2{0.f, 0.f};
        *reinterpret_cast<Vec2*>(base() + kMove) = Vec2{0.f, 0.f};
        *reinterpret_cast<Vec2*>(base() + kRawMove) = Vec2{0.f, 0.f};
    }

private:
    uintptr_t base() { return reinterpret_cast<uintptr_t>(this); }
    uintptr_t base() const { return reinterpret_cast<uintptr_t>(this); }
};

} // namespace SDK
