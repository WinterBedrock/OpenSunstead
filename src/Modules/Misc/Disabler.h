#pragma once
#include "../Module.h"
#include <cstdint>
#include <deque>

namespace SDK { class Packet; }

class Disabler : public Module {
public:
    Disabler() : Module("Disabler", "Disrupts server-side movement prediction", Category::Misc, 0) {
        AddSetting(new EnumSetting("Mode", "Which prediction check to disrupt", 0,
            { "Drift", "Phase", "Tick Freeze", "Edge Clash", "Glide", "Null Motion",
              "Hive Motion", "Ground Lock", "Tick Slip", "Latency Hold" }));  // [0]
        AddSetting(new NumberSetting("Slip", "How many ticks Tick Slip reports behind",
                                     80.0f, 10.0f, 200.0f, 5.0f));              // [1]
        AddSetting(new NumberSetting("Hold", "Latency replies kept before a batch flush",
                                     12.0f, 2.0f, 40.0f, 1.0f));                 // [2]
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnTick() override;
    void OnPacketSend(SDK::Packet* packet, int packetId, bool& cancel) override;

    int GetMode() const { return static_cast<EnumSetting*>(mSettings[0])->mValue; }

    // Called from the PAI branch of the LoopbackPacketSender::send hook,
    // BEFORE silent aim rewrites the rotation.
    void ModifyAuthInput(void* packet);

    // Hive Motion, called AFTER silent aim. `realYaw` is the head yaw captured
    // before that rewrite; the packet's yaw is already the spoofed one.
    void AlignMove(void* packet, float realYaw);

private:
    int mLastMode = -1;
    long long mMppSeq = 0;
    // Tick Slip remembers the first real client tick it saw and then counts
    // its own, so the reported tick stays a constant distance behind.
    long long mSlipBase = -1;
    long long mSlipCount = 0;

    struct HeldLatency { uint64_t time; bool fromServer; };
    std::deque<HeldLatency> mHeld;
    bool mFlushing = false;
    static bool sFlushing;

    void flushLatency();

public:
    // The latency flush re-enters PacketSender::send. The send hook skips its
    // module pipeline while this is set, so the flush cannot recurse.
    static bool IsFlushing();
};
