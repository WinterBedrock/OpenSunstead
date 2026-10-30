#include "Keystrokes.h"
#include "../../Render/HudEditor.h"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Utils/MiscUtils/MathUtils.hpp"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../../Utils/MiscUtils/D2D.hpp"
#include "../../Utils/MCSDK.h"
#include <Windows.h>

void drawKeyBox(float x, float y, float w, float h, const std::string& label, float animValue, ImColor themeColor, ImColor customTextColor, float scale, float blur, float dim, float rounding) {
    ImVec4 rect(x, y, x + w, y + h);
    
    ImColor bgIdleCol = ImColor(25, 25, 28, (int)(255 * dim));
    ImColor bgCol = ColorUtils::LerpImColor(bgIdleCol, themeColor, animValue);
    bgCol.Value.w = dim + (1.0f - dim) * animValue; 

    ImColor textCol = ColorUtils::LerpImColor(customTextColor, ImColor(0, 0, 0), animValue);
    
    if (blur > 0.01f) {
        D2D::addBlurOptimized(ImGui::GetBackgroundDrawList(), blur, std::optional<ImVec4>(rect), rounding * scale);
    }
    
    // Soft shadow — deliberately faint: a small spread and low density so the
    // little key plates don't read as heavy black slabs.
    ImRenderUtils::fillShadowRectangle(rect, ImColor(10, 10, 10), 0.22f + 0.16f * animValue, rounding * scale, 6.0f * scale, 0.45f);
    ImRenderUtils::fillRectangle(rect, bgCol, 1.0f, rounding * scale);
    
    float fontSize = 14.0f * scale;
    float labelW = ImRenderUtils::getTextWidth(&label, fontSize);
    ImRenderUtils::drawText(ImVec2(x + (w - labelW) / 2.0f, y + (h - fontSize) / 2.0f), label, textCol, fontSize, 1.0f, animValue < 0.5f);
}

void Keystrokes::OnRender() {
    if (!IsEnabled()) return;
    // Also visible while the HUD editor is open (it runs on the pause/chat screens).
    if (!HudEditor::HudVisible()) return;

    float scale = mScale;

    mWidth = 98.0f * scale;
    mHeight = 115.0f * scale;

    EnumSetting* style = static_cast<EnumSetting*>(mSettings[0]);
    ColorSetting* customCol = static_cast<ColorSetting*>(mSettings[1]);
    NumberSetting* blurSet = static_cast<NumberSetting*>(mSettings[2]);
    NumberSetting* dimSet = static_cast<NumberSetting*>(mSettings[3]);
    NumberSetting* roundSet = static_cast<NumberSetting*>(mSettings[4]);

    ImColor themeCol = (style->mValue == 0) 
        ? ColorUtils::getThemedColor(0) 
        : ImColor(customCol->mValue[0], customCol->mValue[1], customCol->mValue[2], customCol->mValue[3]);

    ImColor textCol(255, 255, 255);
    float dt = ImRenderUtils::getDeltaTime();

    bool states[7] = {
        (GetAsyncKeyState('W') & 0x8000) != 0,
        (GetAsyncKeyState('A') & 0x8000) != 0,
        (GetAsyncKeyState('S') & 0x8000) != 0,
        (GetAsyncKeyState('D') & 0x8000) != 0,
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0,
        (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0,
        (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0
    };

    for (int i = 0; i < 7; i++) {
        keyAnims[i] = MathUtils::animate(states[i] ? 1.0f : 0.0f, keyAnims[i], dt * 15.0f);
    }

    float b = blurSet->mValue;
    float d = dimSet->mValue;
    float r = roundSet->mValue;

    // Draw Keys
    drawKeyBox(mPosX + 34.0f * scale, mPosY, 30.0f * scale, 30.0f * scale, "W", keyAnims[0], themeCol, textCol, scale, b, d, r);
    drawKeyBox(mPosX, mPosY + 34.0f * scale, 30.0f * scale, 30.0f * scale, "A", keyAnims[1], themeCol, textCol, scale, b, d, r);
    drawKeyBox(mPosX + 34.0f * scale, mPosY + 34.0f * scale, 30.0f * scale, 30.0f * scale, "S", keyAnims[2], themeCol, textCol, scale, b, d, r);
    drawKeyBox(mPosX + 68.0f * scale, mPosY + 34.0f * scale, 30.0f * scale, 30.0f * scale, "D", keyAnims[3], themeCol, textCol, scale, b, d, r);
    drawKeyBox(mPosX, mPosY + 68.0f * scale, 47.0f * scale, 25.0f * scale, "LMB", keyAnims[4], themeCol, textCol, scale, b, d, r);
    drawKeyBox(mPosX + 51.0f * scale, mPosY + 68.0f * scale, 47.0f * scale, 25.0f * scale, "RMB", keyAnims[5], themeCol, textCol, scale, b, d, r);
    drawKeyBox(mPosX, mPosY + 97.0f * scale, 98.0f * scale, 18.0f * scale, "Space", keyAnims[6], themeCol, textCol, scale, b, d, r);
}