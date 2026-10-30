#include "Freelook.h"
#include "../Setting.h"
#include "../../SDK/Client/ScreenView.h"
#include <Windows.h>

Freelook::Freelook()
    : Module("Freelook", "Free camera (look around with your body frozen)", Category::Player, 'F') {
    // [0] Bind Type: Toggle (default) or Hold.
    AddSetting(new EnumSetting("Bind Type", "How the keybind activates Freelook", 0, { "Toggle", "Hold" }));
    canBind = true;
}

// Active state consumed by the camera / perspective hooks.
bool Freelook::IsActive() {
    int bt = static_cast<EnumSetting*>(mSettings[0])->mValue;
    if (bt == 0) return IsEnabled();   // Toggle: standard enabled state
    return mActive;                    // Hold: driven by OnTick polling
}

void Freelook::OnTick() {
    int bt = static_cast<EnumSetting*>(mSettings[0])->mValue;
    bool inGame = SDK::isGameplayInputActive();

    if (bt == 1) {
        // HOLD: ModuleManager::PollHoldBinds force-enables the module while the
        // key is physically held (even when it is toggled off) and force-
        // disables it on release, so IsEnabled() ALREADY carries the whole key
        // state — and a manual ClickGUI enable counts too: toggling the module
        // on from the menu must engage the free camera, not just the key.
        mHoldMode = true;
        mActive = IsEnabled() && inGame;
        mPrevKey = mHoldKeyDown;
    } else {
        // TOGGLE: hand control back to the standard keybind dispatch, which
        // flips 'enabled' on each press. Nothing to poll here.
        mHoldMode = false;
        mActive = false;
    }
}

void Freelook::OnDisable() {
    mActive = false;
}
