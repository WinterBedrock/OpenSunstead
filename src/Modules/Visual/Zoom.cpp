#include "Zoom.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/Client/ScreenView.h"
#include "../../SDK/Client/LevelRendererPlayer.h"
#include "../../Utils/MiscUtils/MathUtils.hpp"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Render/FrameView.h"
#include <Windows.h>
#include <cmath>

// Definition of the shared zoom projection scale declared in FrameView.h.
// Ownership flag ONLY (see the FrameView.h comment): Sprint reads it to stay
// off the FOV fields while Zoom is magnifying. World-to-screen follows the
// FOV through the LRP matrix capture and never reads this.
volatile float g_zoomProjScale = 1.0f;

// Modifier -> focal scale. Positive passes straight through (15 = 15x
// focal). Negative WIDENS the FOV: 1/(1-v) maps (-inf, 0) onto (0, 1), so 0
// is the natural "no zoom" point (focal 1) and -10 gives an 11x-wide FOV —
// exactly what the slider's negative range is for.
static float focalOf(float mod) {
    const float f = mod > 0.0f ? mod : (1.0f / (1.0f - mod));
    return std::clamp(f, 0.05f, 100.0f);
}

void Zoom::OnTick() {
    // Bind Type lives in mSettings[2]; mHoldMode is kept in sync centrally by
    // ModuleManager::PollHoldBinds (it also force-enables us while the key is
    // held, so the zoom reacts even when the module itself is toggled off).
    int bt = static_cast<EnumSetting*>(mSettings[2])->mValue;

    // Zoom must only engage on the in-game HUD. Otherwise a toggle bind would
    // keep zooming through chat/menus/inventory etc.
    bool inGame = SDK::isGameplayInputActive();

    // In Hold mode the poll force-enables us only while the key is down, so
    // IsEnabled() carries the whole key state — AND a manual ClickGUI enable
    // counts too: toggling the module on from the menu must zoom, not just
    // the key. In Toggle mode IsEnabled() is the plain on/off state.
    mHoldMode = (bt == 1);
    mShouldZoom = IsEnabled() && inGame;

    if (!mShouldZoom) {
        // Zooming out: leave the animation to the release in OnRender (it
        // keeps running while disabled). Instant when Smooth is off.
        if (!static_cast<BoolSetting*>(mSettings[1])->mValue) {
            mCurrentFocal = 1.0f;
            g_zoomProjScale = 1.0f;
        }
        return;
    }

    const float target = focalOf(static_cast<NumberSetting*>(mSettings[0])->mValue);
    if (static_cast<BoolSetting*>(mSettings[1])->mValue) {
        mCurrentFocal = MathUtils::lerp(mCurrentFocal, target,
                                         ImRenderUtils::getDeltaTime() * 10.0f);
        if (std::fabs(mCurrentFocal - target) < 0.01f) mCurrentFocal = target;
    } else mCurrentFocal = target;

    g_zoomProjScale = mCurrentFocal;
}

void Zoom::OnRender() {
    // Release animation: OnTick stops running the moment the module disables
    // (hold key release force-disable / toggle off), so the zoom-out is
    // finished here — the same fast clock as the entrance, gliding straight
    // back to focal 1 (normal FOV) with no wide-FOV overshoot on the way.
    if (mShouldZoom || mCurrentFocal == 1.0f) return;
    if (static_cast<BoolSetting*>(mSettings[1])->mValue) {
        mCurrentFocal = MathUtils::lerp(mCurrentFocal, 1.0f,
                                         ImRenderUtils::getDeltaTime() * 10.0f);
        if (std::fabs(mCurrentFocal - 1.0f) < 0.01f) mCurrentFocal = 1.0f;
    } else {
        mCurrentFocal = 1.0f;
    }
    g_zoomProjScale = mCurrentFocal;
}

void Zoom::OnDisable() {
    mShouldZoom = false;
    // mCurrentFocal / g_zoomProjScale are deliberately NOT snapped here: the
    // release animation in OnRender glides the zoom back out fast, and it
    // keeps running because ModuleManager renders Zoom while disabled.
}
