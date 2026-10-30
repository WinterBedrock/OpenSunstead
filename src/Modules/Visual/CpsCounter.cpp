#include "CpsCounter.h"
#include "../../Render/HudEditor.h"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../../Utils/MiscUtils/D2D.hpp"
#include "../../Utils/MCSDK.h"
#include <chrono>
#include <Windows.h>
#include <string>

static double getCpsTime() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now.time_since_epoch()).count();
}

int CpsCounter::GetCPS(std::vector<double>& clickList) {
    double now = getCpsTime();
    while (!clickList.empty() && now - clickList.front() > 1.0) {
        clickList.erase(clickList.begin());
    }
    return static_cast<int>(clickList.size());
}

void CpsCounter::OnRender() {
    if (!IsEnabled()) return;
    // Also visible while the HUD editor is open (it runs on the pause/chat screens).
    if (!HudEditor::HudVisible()) return;

    bool lmb = GetAsyncKeyState(VK_LBUTTON) & 0x8000;
    if (lmb) {
        if (!wasLmbDown) {
            lLmbClicks.push_back(getCpsTime());
            wasLmbDown = true;
        }
    } else {
        wasLmbDown = false;
    }

    bool rmb = GetAsyncKeyState(VK_RBUTTON) & 0x8000;
    if (rmb) {
        if (!wasRmbDown) {
            lRmbClicks.push_back(getCpsTime());
            wasRmbDown = true;
        }
    } else {
        wasRmbDown = false;
    }

    int lCps = GetCPS(lLmbClicks);
    int rCps = GetCPS(lRmbClicks);

    EnumSetting* mode = static_cast<EnumSetting*>(mSettings[2]);

    std::string value;
    if (mode->mValue == 0) {
        value = std::to_string(lCps);
    } else if (mode->mValue == 1) {
        value = std::to_string(rCps);
    } else {
        value = std::to_string(lCps) + " | " + std::to_string(rCps);
    }

    DrawCounterModule(this, value, "CPS", 18.0f);
}
