#pragma once
#include "../Module.h"

namespace SDK { class Actor; }

// Sprint — holds sprint ON while walking forward.
//
// The game rebuilds MoveInputComponent from the keyboard inside
// ClientInputUpdateSystem::tick, so a write from OnTick lands before that
// rebuild and gets wiped on the next input pass (sprint on one tick, off the
// next). The write now happens in a hook that runs AFTER that tick, into the
// same place Latite's toggle-sprint writes: rawInputState.sprintDown, which is
// bit 8 of the flag dword at +0x10 (the dword Baritone already writes).
class Sprint : public Module {
public:
    Sprint() : Module("Sprint", "Automatically sprints when moving forward", Category::Movement, 0) {
        AddSetting(new BoolSetting("FOV Effect", "Widen the field of view a little while moving forward, like vanilla sprinting", true));  // [0]
        AddSetting(new NumberSetting("FOV Amount", "FOV multiplier while moving (lower = wider view)", 0.92f, 0.85f, 1.0f, 0.01f));          // [1]
    }

    void OnTick() override;
    void OnUpdate() override;   // game thread
    void OnEnable() override;
    void OnDisable() override;

    // Called from the renderLevel hook right before the world render. The
    // input hook owns the sprint bit now; this stays for hook compatibility.
    void PreRenderAssert() {}

    // After ClientInputUpdateSystem::tick has rebuilt the component.
    static void AfterInputTick();

    // Installs the shared ClientInputUpdateSystem::tick hook (idempotent).
    // GhostHand's pick override rides the same hook, so it must be creatable
    // from outside too.
    static void EnsureInputHook();
    // True while the shared ClientInput tick hook is live.
    static bool HookInstalled();

private:
    void applySprintInput(SDK::Actor* lp);
    void applyFovEffect();
};
