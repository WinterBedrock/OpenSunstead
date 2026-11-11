#include "Watermark.h"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../../Utils/MiscUtils/D2D.hpp"
#include "../../Core/Sunstead.h"
#include <time.h>
#include <string>

static std::string getWatermarkTime() {
    time_t rawtime;
    struct tm timeinfo;
    time(&rawtime);
    localtime_s(&timeinfo, &rawtime);
    char buffer[10];
    strftime(buffer, sizeof(buffer), "%H:%M", &timeinfo);
    return std::string(buffer);
}

void Watermark::OnRender() {
    if (!IsEnabled()) return;

    BoolSetting* glowSet = static_cast<BoolSetting*>(mSettings[0]);
    NumberSetting* glowStrengthSet = static_cast<NumberSetting*>(mSettings[1]);
    BoolSetting* showInfo = static_cast<BoolSetting*>(mSettings[2]);
    NumberSetting* textShadowSet = static_cast<NumberSetting*>(mSettings[3]);

    std::string text = Sunstead::ClientName;
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    if (showInfo->mValue) {
        int fps = (int)ImGui::GetIO().Framerate;
        text += " | Dev | " + std::to_string(fps) + " FPS | " + getWatermarkTime();
    }

    float scale = mScale;
    float fontSize = 18.0f * scale;
    float textWidth = ImRenderUtils::getTextWidth(&text, fontSize);

    bool glow = glowSet->mValue;
    float glowStrength = glowStrengthSet->mValue;
    float textShadow = textShadowSet->mValue;

    mWidth = textWidth;
    mHeight = 22.0f * scale;

    if (glow) {
        ImRenderUtils::drawGlowText(ImVec2(mPosX, mPosY), text, fontSize, 1.0f,
            [&](float index){ return ColorUtils::getThemedColor(index * 22.0f); },
            15.0f * scale, 0.55f * glowStrength);
    }
    if (textShadow > 0.001f) {
        ImRenderUtils::drawText(ImVec2(mPosX + 2.0f * scale, mPosY + 2.0f * scale),
            text, ImColor(0, 0, 0), fontSize, textShadow, false);
    }
    ImRenderUtils::drawGradientText(ImVec2(mPosX, mPosY), text, fontSize, 1.0f, false, [](float index) {
        return ColorUtils::getThemedColor(index * 22.0f);
    });
}
