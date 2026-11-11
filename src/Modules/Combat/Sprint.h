#pragma once
#include "../Module.h"

namespace SDK { class Actor; }

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

    static void EnsureInputHook();
    // True while the shared ClientInput tick hook is live.
    static bool HookInstalled();

private:
    void applySprintInput(SDK::Actor* lp);
    void applyFovEffect();
};
