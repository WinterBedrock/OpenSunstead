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

    EnumSetting* colorMode = static_cast<EnumSetting*>(mSettings[0]);
    BoolSetting* glowSet = static_cast<BoolSetting*>(mSettings[1]);
    NumberSetting* colorsNum = static_cast<NumberSetting*>(mSettings[2]);
    NumberSetting* glowStrengthSet = static_cast<NumberSetting*>(mSettings[9]);
    BoolSetting* showInfo = static_cast<BoolSetting*>(mSettings[10]);
    NumberSetting* textShadowSet = static_cast<NumberSetting*>(mSettings[11]);
    float textShadow = textShadowSet->mValue;

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

    auto getGradColor = [&](float index) {
        if (colorMode->mValue == 0) { // Theme
            return ColorUtils::getThemedColor(index * 22.0f);
        } else if (colorMode->mValue == 1) { // Custom
            std::vector<ImColor> customColors;
            int numColors = (int)colorsNum->mValue;
            for (int c = 1; c <= numColors; c++) {
                auto* cSet = static_cast<ColorSetting*>(mSettings[2 + c]);
                customColors.push_back(ImColor(cSet->mValue[0], cSet->mValue[1], cSet->mValue[2], cSet->mValue[3]));
            }
            return ColorUtils::LerpColors(2.0f, index, customColors);
        } else { // Rainbow
            return ColorUtils::Rainbow(2.0f, 1.0f, 1.0f, (int)index);
        }
    };

    bool glow = glowSet->mValue;
    float glowStrength = glowStrengthSet->mValue;

    // Plain style only.
    mWidth = textWidth;
    mHeight = 22.0f * scale;
    if (glow) {
        ImRenderUtils::drawGlowText(ImVec2(mPosX, mPosY), text, fontSize, 1.0f,
            [&](float index){ return getGradColor(index); }, 15.0f * scale, 0.55f * glowStrength);
    }
    if (textShadow > 0.001f) {
        ImRenderUtils::drawText(ImVec2(mPosX + 2.0f * scale, mPosY + 2.0f * scale), text, ImColor(0, 0, 0), fontSize, textShadow, false);
    }
    ImRenderUtils::drawGradientText(ImVec2(mPosX, mPosY), text, fontSize, 1.0f, false, [&](float index) {
        return getGradColor(index);
    });
}