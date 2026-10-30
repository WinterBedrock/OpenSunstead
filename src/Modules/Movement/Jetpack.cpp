#include "Jetpack.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/SDK.h"
#include <cmath>
#include <Windows.h>

#define PI 3.14159265358979323846f

void Jetpack::OnTick() {
    int bindType = static_cast<EnumSetting*>(mSettings[3])->mValue;

    if (bindType == 1) { // Hold
        // ModuleManager::PollHoldBinds force-enables us while the keybind is
        // physically held (even when the module is toggled off) and force-
        // disables it on release, so IsEnabled() ALREADY carries the whole
        // key state — and a manual ClickGUI enable counts too: toggling the
        // module on from the menu must fly, not just the key. No
        // self-enable here: that is what used to pin the module "on".
        mHoldMode = true;
        if (!IsEnabled()) return;
    } else {
        mHoldMode = false;
        if (!IsEnabled()) return;
    }

    // Only work in HUD screen (stops working in inventory/menus)
    if (!SDK::isGameplayInputActive()) return;

    SDK::ClientInstance* cInst = SDK::ClientInstance::get();
    if (!cInst) return;
    SDK::Actor* lp = cInst->getLocalPlayer();
    if (!lp) return;

    auto* stateVec = lp->getStateVector();
    if (!stateVec) return;

    Vec2 rot = lp->getRot();
    float calcYaw = (rot.y + 90.0f) * (PI / 180.0f);
    float calcPitch = rot.x * -(PI / 180.0f);

    if (static_cast<BoolSetting*>(mSettings[4])->mValue) {
        // BYPASS: glide-band flight. Vertical velocity is FORCED into the
        // vanilla-elytra drift band (-0.08..-0.01 per tick) regardless of look
        // pitch — servers flag rocket-class upward velocity, but accept
        // glide-class downward drift everywhere. Horizontal speed is clamped
        // low. The look direction only steers the horizontal component.
        float hSpeed = static_cast<NumberSetting*>(mSettings[0])->mValue;
        if (hSpeed > 1.2f) hSpeed = 1.2f;
        float glide = static_cast<NumberSetting*>(mSettings[5])->mValue;
        if (glide > -0.01f) glide = -0.01f;
        if (glide < -0.08f) glide = -0.08f;

        stateVec->velocity.x = cos(calcYaw) * hSpeed;
        stateVec->velocity.y = glide;
        stateVec->velocity.z = sin(calcYaw) * hSpeed;
        return;
    }

    float hSpeed = static_cast<NumberSetting*>(mSettings[0])->mValue;
    float vSpeed = static_cast<NumberSetting*>(mSettings[1])->mValue;

    stateVec->velocity.x = cos(calcYaw) * cos(calcPitch) * hSpeed;
    stateVec->velocity.y = sin(calcPitch) * vSpeed;
    stateVec->velocity.z = sin(calcYaw) * cos(calcPitch) * hSpeed;
}

void Jetpack::OnDisable() {
    bool reset = static_cast<BoolSetting*>(mSettings[2])->mValue;
    if (reset) {
        SDK::ClientInstance* cInst = SDK::ClientInstance::get();
        if (cInst) {
            SDK::Actor* lp = cInst->getLocalPlayer();
            if (lp) {
                auto* stateVec = lp->getStateVector();
                if (stateVec) {
                    stateVec->velocity.x = 0;
                    stateVec->velocity.y = 0;
                    stateVec->velocity.z = 0;
                }
            }
        }
    }
}
