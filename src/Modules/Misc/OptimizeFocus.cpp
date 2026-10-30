#include "OptimizeFocus.h"
#include "../Setting.h"
#include "../../SDK/Runtime/Guard.h"
#include <imgui.h>
#include <Windows.h>

void OptimizeFocus::OnRender() {
    if (!IsEnabled()) return;
    if (SDK::isGameWindowFocused()) return; // focused: do nothing, full performance

    float fpsCap   = static_cast<NumberSetting*>(mSettings[0])->mValue;
    bool  dim      = static_cast<BoolSetting*>(mSettings[1])->mValue;
    float dimAmt   = static_cast<NumberSetting*>(mSettings[2])->mValue;
    bool  showText = static_cast<BoolSetting*>(mSettings[3])->mValue;

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 screen = io.DisplaySize;
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    if (dim && dimAmt > 0.001f) {
        ImU32 shade = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, dimAmt));
        dl->AddRectFilled(ImVec2(0, 0), screen, shade);
    }

    if (showText) {
        const char* msg = "Window not focused";
        float fontSize = 32.0f;
        ImFont* font = ImGui::GetFont();
        ImVec2 ts = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, msg);
        ImVec2 pos((screen.x - ts.x) * 0.5f, (screen.y - ts.y) * 0.5f);
        // soft shadow for readability
        dl->AddText(font, fontSize, ImVec2(pos.x + 2, pos.y + 2),
                    ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.7f)), msg);
        dl->AddText(font, fontSize, pos,
                    ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 0.9f)), msg);
    }

    // Throttle the render thread to the FPS cap. Sleeping here slows the whole
    // present loop while unfocused, which is exactly the resource saving we want.
    if (fpsCap < 1.0f) fpsCap = 1.0f;
    DWORD sleepMs = (DWORD)(1000.0f / fpsCap);
    Sleep(sleepMs);
}
