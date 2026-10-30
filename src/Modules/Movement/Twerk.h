#pragma once
#include "../Module.h"
#include <Windows.h>
#include <atomic>

// Twerk — crouch spam. Uses MoveInput (correct entt hash) + AuthInput START/STOP
// sneak flags so both client visual and server see the pulse.
class Twerk : public Module {
public:
    Twerk() : Module("Twerk", "Rapid crouch spam (sit/stand)", Category::Movement, 0) {
        AddSetting(new NumberSetting("Delay", "Milliseconds between crouch toggles", 80.f, 20.f, 500.f, 5.f)); // [0]
    }

    void OnEnable() override;
    void OnDisable() override;
    void OnTick() override;
    void OnUpdate() override;

    // Read by GameHooks::hkPacketSend to inject AuthInput sneak bits.
    static bool WantSneak();
    static bool ConsumeEdge(bool& start, bool& stop);

private:
    float GetDelay() const { return static_cast<NumberSetting*>(mSettings[0])->mValue; }

    bool mSneaking = false;
    bool mPrevSneaking = false;
    unsigned long long mLastToggleMs = 0;

    static std::atomic<bool> s_wantSneak;
    static std::atomic<int>  s_edge; // 1=start, 2=stop, 0=none

    static bool applyMoveInputSneak(bool down);
};
