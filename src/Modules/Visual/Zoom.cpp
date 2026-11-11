#include "Zoom.h"
#include "../../Utils/MCSDK.h"
#include "../../SDK/Client/ScreenView.h"
#include "../../SDK/Client/LevelRendererPlayer.h"
#include "../../Utils/MiscUtils/MathUtils.hpp"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Render/FrameView.h"
#include <Windows.h>
#include <cmath>

volatile float g_zoomProjScale = 1.0f;

static float focalOf(float mod) {
    const float f = mod > 0.0f ? mod : (1.0f / (1.0f - mod));
    return std::clamp(f, 0.05f, 100.0f);
}

void Zoom::OnTick() {
    int bt = static_cast<EnumSetting*>(mSettings[2])->mValue;

    // Zoom must only engage on the in-game HUD. Otherwise a toggle bind would
    // keep zooming through chat/menus/inventory etc.
    bool inGame = SDK::isGameplayInputActive();

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
}
