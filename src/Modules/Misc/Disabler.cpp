#include "Disabler.h"
#include "../Setting.h"
#include "../../SDK/Network/Packet.h"
#include "../../SDK/Network/PacketUtils.h"
#include "../../SDK/Network/Packets/PlayerAuthInputPacket.h"
#include <Windows.h>
#include <cmath>
#include <deque>
#include <memory>

// MovePlayerPacket (ID 19) — 1.26 layout: ActorRuntimeID @ 0x30, mPos @ 0x38,
// mOnGround @ 0x51 (0x51 is verified — GameHooks and NoFall spoof it).
static constexpr uintptr_t kMP_RuntimeID = 0x30;
static constexpr uintptr_t kMP_OnGround  = 0x51;
static constexpr size_t    kMP_Span      = 0x52;

// NetworkStackLatencyPacket (ID 115): mCreateTime @ 0x30, mFromServer @ 0x38.
static constexpr uintptr_t kNSL_Time = 0x30;
static constexpr uintptr_t kNSL_From = 0x38;
static constexpr int kNSL_Id = 115;

void Disabler::OnEnable() {
    mLastMode = -1;
    mMppSeq = 0;
    mSlipBase = -1;
    mSlipCount = 0;
    mHeld.clear();
}

void Disabler::OnDisable() {
    flushLatency();
    mLastMode = -1;
    mMppSeq = 0;
    mSlipBase = -1;
    mSlipCount = 0;
}

// Hive Motion: rebuild the reported move so it agrees with the reported yaw.
//
// The packet carries the movement the client integrated against its REAL yaw
// (mMove). Silent aim then overwrites mRot AND mYHeadRot with the spoofed yaw,
// so the real one has to be captured before that rewrite and passed in here.
// When the two frames differ the server reproduces a different world-space step
// than the client took and flags the gap. Rotating the move vector into the
// reported frame makes both sides agree.
//
// mMove is (strafe, forward). The world step it produced is recovered with the
// real yaw, then re-expressed against the reported yaw.
static void alignMoveToYaw(SDK::PlayerAuthInputPacket* pai, float realYaw) {
    const float strafe = pai->moveVec().x;
    const float fwd    = pai->moveVec().y;
    if (strafe == 0.f && fwd == 0.f) return;

    const float reported = pai->yaw();
    if (realYaw == reported) return;

    constexpr float kD2R = 3.141592653589793f / 180.0f;
    const float ang = std::atan2(strafe, fwd);
    const float world = (realYaw + ang * 180.0f / 3.141592653589793f + 90.0f) * kD2R;
    const float wx = std::cos(world);
    const float wz = std::sin(world);
    const float sy = reported * kD2R;
    const float fx = -std::sin(sy), fz = std::cos(sy);
    const float sx =  std::cos(sy), sz = std::sin(sy);
    const float outFwd    = wx * fx + wz * fz;
    const float outStrafe = wx * sx + wz * sz;

    const Vec2 aligned{ outStrafe, outFwd };
    pai->moveVec() = aligned;
    pai->analogMoveVec() = aligned;
    pai->rawMoveVec() = aligned;

    using A = SDK::AuthInputAction;
    pai->removeInput(A::UP | A::DOWN | A::LEFT | A::RIGHT |
                     A::UP_LEFT | A::UP_RIGHT | A::DOWN_LEFT | A::DOWN_RIGHT);
    if (outFwd    >  0.3f) pai->addInput(A::UP);
    if (outFwd    < -0.3f) pai->addInput(A::DOWN);
    if (outStrafe >  0.3f) pai->addInput(A::LEFT);
    if (outStrafe < -0.3f) pai->addInput(A::RIGHT);
}

void Disabler::ModifyAuthInput(void* packet) {
    if (!IsEnabled() || !packet) return;
    const int mode = GetMode();
    if (mode != mLastMode) {
        mLastMode = mode;
        mSlipBase = -1;
        mSlipCount = 0;
    }

    __try {
        if (!sdk_safe_ptr(packet, 0xB0)) return;
        auto* pai = reinterpret_cast<SDK::PlayerAuthInputPacket*>(packet);
        static long long s_seq = 0;
        const long long tick = s_seq++;

        switch (mode) {
        case 0: { // Drift
            const float perc = static_cast<float>(tick % 3) / 3.0f;
            const float targetY = (perc < 0.5f) ? 0.02f : -0.02f;
            pai->pos().y += targetY * perc;
            pai->moveVec().y = -(1.0f / 3.0f);
            pai->addInput(SDK::AuthInputAction::JUMPING);
            if (tick % 3 == 0)
                pai->addInput(SDK::AuthInputAction::START_JUMPING);
            break;
        }
        case 1: { // Phase
            const float phase = static_cast<float>(tick % 4) / 4.0f;
            const float offset = (phase < 0.5f) ? 0.025f : -0.025f;
            pai->pos().y += offset * phase;
            pai->moveVec().y = -(1.0f / 4.0f);
            break;
        }
        case 2: // Tick Freeze
            pai->clientTick() = 0;
            break;
        case 3: { // Edge Clash
            pai->addInput(SDK::AuthInputAction::START_SNEAKING);
            if (!pai->hasInput(SDK::AuthInputAction::SNEAKING))
                pai->addInput(SDK::AuthInputAction::STOP_SNEAKING);
            break;
        }
        case 4: { // Glide
            using A = SDK::AuthInputAction;
            pai->addInput(A::START_GLIDING | A::JUMPING | A::WANT_UP | A::JUMP_DOWN |
                          A::NORTH_JUMP_DEPRECATED | A::START_JUMPING |
                          A::SPRINT_DOWN | A::SPRINTING | A::START_SPRINTING);
            pai->removeInput(A::STOP_GLIDING | A::STOP_SPRINTING | A::SNEAK_DOWN |
                             A::SNEAKING | A::START_SNEAKING | A::STOP_SNEAKING);
            break;
        }
        case 5: // Null Motion
            pai->pos() = Vec3{0.f, 0.f, 0.f};
            pai->moveVec()       = Vec2{0.f, 0.f};
            pai->analogMoveVec() = Vec2{0.f, 0.f};
            pai->rawMoveVec()    = Vec2{0.f, 0.f};
            break;
        case 6: // Hive Motion — applied later, once silent aim has written
            break;
        case 7: // Ground Lock
            pai->spoofOnGround();
            break;
        case 8: { // Tick Slip
            const long long real = pai->clientTick();
            if (mSlipBase < 0) mSlipBase = real;
            const long long behind = (long long)static_cast<NumberSetting*>(mSettings[1])->mValue;
            const long long reported = mSlipBase + mSlipCount - behind;
            pai->clientTick() = reported < 0 ? 0 : reported;
            ++mSlipCount;
            break;
        }
        default:
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void Disabler::AlignMove(void* packet, float realYaw) {
    if (!IsEnabled() || !packet || GetMode() != 6) return;
    __try {
        if (!sdk_safe_ptr(packet, 0xB0)) return;
        alignMoveToYaw(reinterpret_cast<SDK::PlayerAuthInputPacket*>(packet), realYaw);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// MovePlayerPacket side of the modes that also have to agree with it. Modern
// Bedrock sends it almost only on teleports, but the servers these modes face
// still read it.
static bool sehModifyMPP(void* packet, int mode, long long seq) {
    __try {
        if (!sdk_safe_ptr(packet, kMP_Span)) return true;
        const uintptr_t base = reinterpret_cast<uintptr_t>(packet);
        switch (mode) {
        case 0:
            *reinterpret_cast<bool*>(base + kMP_OnGround) = true;
            break;
        case 1:
            *reinterpret_cast<bool*>(base + kMP_OnGround) = true;
            if (seq % 5 == 0)
                *reinterpret_cast<uint64_t*>(base + kMP_RuntimeID) = 0;
            break;
        case 2:
            *reinterpret_cast<uint64_t*>(base + kMP_RuntimeID) = 0;
            break;
        default:
            break;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}

static bool sehReadLatency(void* packet, uint64_t* time, bool* fromServer) {
    __try {
        if (!sdk_safe_ptr(packet, kNSL_From + 1)) return false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(packet);
        *time = *reinterpret_cast<uint64_t*>(base + kNSL_Time);
        *fromServer = *reinterpret_cast<bool*>(base + kNSL_From);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// POD only: __try cannot share a function with a std::shared_ptr (C2712).
static bool sehWriteLatency(void* packet, uint64_t time, bool fromServer) {
    __try {
        if (!sdk_safe_ptr(packet, kNSL_From + 1)) return false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(packet);
        *reinterpret_cast<uint64_t*>(base + kNSL_Time) = time;
        *reinterpret_cast<bool*>(base + kNSL_From) = fromServer;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool Disabler::sFlushing = false;

bool Disabler::IsFlushing() { return sFlushing; }

void Disabler::flushLatency() {
    if (mHeld.empty() || mFlushing) return;
    mFlushing = true;
    sFlushing = true;
    std::deque<HeldLatency> batch;
    batch.swap(mHeld);
    for (const HeldLatency& h : batch) {
        std::shared_ptr<SDK::Packet> pkt =
            SDK::MinecraftPackets::createPacket((SDK::PacketID)kNSL_Id);
        if (!pkt || !sehWriteLatency(pkt.get(), h.time, h.fromServer)) continue;
        SDK::PacketUtils::sendToServer(pkt.get());
    }
    sFlushing = false;
    mFlushing = false;
}

void Disabler::OnTick() {
    if (!IsEnabled() || GetMode() != 9) return;
    const int cap = (int)static_cast<NumberSetting*>(mSettings[2])->mValue;
    if ((int)mHeld.size() >= cap) flushLatency();
}

void Disabler::OnPacketSend(SDK::Packet* packet, int packetId, bool& cancel) {
    if (!IsEnabled() || !packet) return;

    if (packetId == kNSL_Id && GetMode() == 9 && !mFlushing) {
        uint64_t time = 0; bool fromServer = false;
        if (sehReadLatency(packet, &time, &fromServer)) {
            mHeld.push_back({ time, fromServer });
            cancel = true;
        }
        return;
    }

    if (packetId != static_cast<int>(SDK::PacketID::MovePlayer)) return;
    const int mode = GetMode();
    if (mode > 2) return;
    sehModifyMPP(packet, mode, mMppSeq++);
}
