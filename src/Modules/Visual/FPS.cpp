#include "FPS.h"
#include "../../Render/HudEditor.h"
#include <string>

void FPS::OnRender() {
    if (!IsEnabled()) return;
    // Also visible while the HUD editor is open (it runs on the pause/chat screens).
    if (!HudEditor::HudVisible()) return;

    const int fps = (int)ImGui::GetIO().Framerate;
    DrawCounterModule(this, std::to_string(fps), "FPS");
}
