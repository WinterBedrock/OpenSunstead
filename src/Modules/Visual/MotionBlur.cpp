#include "MotionBlur.h"
#include "../../Utils/MiscUtils/D2D.hpp"
#include "../../Utils/MCSDK.h"
#include <imgui.h>

void MotionBlur::OnEnable() {
    D2D::clearGhostFrames();
}

void MotionBlur::OnDisable() {
    D2D::clearGhostFrames();
}

void MotionBlur::OnRender() {
    if (!IsEnabled()) return;

    // Skip pure menus; allow hud + pause-ish overlays where world is still visible.
    const std::string& sn = SDK::ScreenView::currentScreenName();
    if (sn == "start_screen" || sn == "play_screen" || sn == "settings_screen")
        return;

    float strength = static_cast<NumberSetting*>(mSettings[0])->mValue;
    int   frames   = static_cast<int>(static_cast<NumberSetting*>(mSettings[1])->mValue);
    if (frames < 1) frames = 1;
    if (frames > 20) frames = 20;
    if (strength < 0.01f) strength = 0.01f;

    D2D::addGhostFrame(ImGui::GetForegroundDrawList(), frames, strength);
}
