#pragma once
#include "../Module.h"
#include <cstdint>
#include <deque>

namespace SDK { class Packet; }

// Disabler — outgoing movement spoofing aimed at modern Bedrock prediction
// checks (The Hive's Flareon stack and the BDS-prediction family).
//
// The PlayerAuthInput branch runs from GameHooks::hkPacketSend BEFORE every
// other packet module. The game rebuilds every spoofed field each tick, so
// toggling off returns to vanilla instantly.
//
// Modes:
//   Drift        — small oscillating Y and a backward move vector on a short
//                  cycle, with the jump edge pulsed. A replay that integrates
//                  our own stream can never settle on one path.
//   Phase        — gentler four-tick version of Drift; MovePlayer also drops
//                  its runtime id on a stride.
//   Tick Freeze  — the client tick is held at 0 and MovePlayer's runtime id is
//                  zeroed, so every time-delta check loses its timeline.
//   Edge Clash   — START_SNEAKING is asserted on every packet and STOP_SNEAKING
//                  is added whenever SNEAKING is absent. Prediction rejects a
//                  start that has no matching state.
//   Glide        — gliding, jumping and sprinting are all forced on and their
//                  stop/sneak counterparts stripped, so the predictor switches
//                  to elytra handling and stops applying ground friction.
//   Null Motion  — position and every move vector are zeroed. No speed track
//                  can be built from the stream.
//   Hive Motion  — the reported move vector is rotated into the same frame as
//                  the reported yaw, and the four direction bits are rebuilt
//                  from that vector. Hive's movement check compares the
//                  direction bits against the yaw; a mismatch (silent aim while
//                  walking) is exactly what it flags.
//   Ground Lock  — the vertical delta is the vanilla grounded value and the
//                  jump family is stripped, so fall-distance checks see a
//                  player who never left the ground.
//   Tick Slip    — the client tick is walked a few steps behind the real one.
//                  Reach and timer checks that subtract "now - clientTick"
//                  come out inside the allowed window.
//   Latency Hold — outgoing NetworkStackLatency replies are held and released
//                  in a batch. The Hive combat timer is built from those
//                  timestamps; batching them collapses the window it measures.
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
