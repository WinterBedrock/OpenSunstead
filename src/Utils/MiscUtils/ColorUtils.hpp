#pragma once
#include <vector>
#include <algorithm>
#include <chrono>
#include <string>
#include <cmath>
#include "imgui.h"
#include "../../Modules/ModuleManager.h"

namespace ColorUtils {
    inline uint64_t GetTimeMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    inline ImColor Rainbow(float seconds, float saturation, float brightness, int index) {
        float hue = ((GetTimeMs() + index) % (int)(seconds * 1000)) / (float)(seconds * 1000);
        return ImColor::HSV(hue, saturation, brightness);
    }

    inline ImColor LerpImColor(ImColor a, ImColor b, float t) {
        return ImColor(
            a.Value.x + (b.Value.x - a.Value.x) * t,
            a.Value.y + (b.Value.y - a.Value.y) * t,
            a.Value.z + (b.Value.z - a.Value.z) * t,
            a.Value.w + (b.Value.w - a.Value.w) * t
        );
    }

    inline ImColor LerpColors(float seconds, float index, std::vector<ImColor> colors, uint64_t ms = 0) {
        if (colors.empty()) return ImColor(255, 255, 255, 255);
        if (colors.size() == 1) return colors[0];
        
        float time = 10000.0f / seconds;
        auto angle = static_cast<float>(((ms == 0 ? GetTimeMs() : ms) + static_cast<int>(index)) % static_cast<int>(time));
        float segmentTime = time / colors.size();

        int segmentIndex = static_cast<int>(angle / segmentTime);
        float segmentIndexFloat = angle / segmentTime - segmentIndex;

        ImColor startColor = colors[segmentIndex];
        ImColor endColor = colors[(segmentIndex + 1) % colors.size()];
        return LerpImColor(startColor, endColor, segmentIndexFloat);
    }

    // Wave: vertical sine brightness modulation on top of the base palette.
    // Smooth: plain LerpColors / Rainbow (no extra modulation).
    inline ImColor applyGradientStyle(ImColor base, float index, int style, float speed, uint64_t ms) {
        if (style != 1) return base; // 0=Smooth
        // style 1 = Wave
        const float t = ((ms == 0 ? GetTimeMs() : ms) * 0.001f * speed) + index * 0.045f;
        const float w = 0.55f + 0.45f * (0.5f + 0.5f * sinf(t));
        return ImColor(base.Value.x * w, base.Value.y * w, base.Value.z * w, base.Value.w);
    }

    // The `index` span that corresponds to ONE full palette cycle in
    // getThemedColor. The ClickGUI stretches exactly one period across the
    // whole menu's diagonal, so the spatial gradient stays a full cycle no
    // matter what Color Speed (which only changes how fast it plays) is set to.
    inline float getThemedPeriod() {
        Module* iface = ModuleManager::GetInstance().GetModuleByName("ClickGUI");
        if (iface) {
            int themeIdx = 0;
            float speed = 2.0f;
            for (auto* setting : iface->mSettings) {
                if (!setting) continue;
                if (setting->mName == "Theme") {
                    themeIdx = static_cast<EnumSetting*>(setting)->mValue;
                } else if (setting->mName == "Color Speed") {
                    speed = static_cast<NumberSetting*>(setting)->mValue;
                }
            }
            if (themeIdx == 7) return std::max(200.0f, speed * 1000.0f);   // Spectrum hue cycle
            return std::max(200.0f, 10000.0f / std::max(0.1f, speed));     // LerpColors cycle
        }
        return 5000.0f;
    }

    inline ImColor getThemedColor(float index, uint64_t ms = 0) {
        // Appearance settings live on the ClickGUI module (they used to be their
        // own settings-only "Interface" module). Read by name so the setting order
        // in the ctor is free to change.
        Module* iface = ModuleManager::GetInstance().GetModuleByName("ClickGUI");
        if (iface) {
            int themeIdx = 0;
            int gradStyle = 0; // 0=Smooth 1=Wave
            float speed = 2.0f;
            float sat = 1.0f;
            int numColors = 3;
            std::vector<ImColor> customColors;

            for (auto* setting : iface->mSettings) {
                if (!setting) continue;
                if (setting->mName == "Theme") {
                    themeIdx = static_cast<EnumSetting*>(setting)->mValue;
                } else if (setting->mName == "Gradient") {
                    gradStyle = static_cast<EnumSetting*>(setting)->mValue;
                } else if (setting->mName == "Color Speed") {
                    speed = static_cast<NumberSetting*>(setting)->mValue;
                } else if (setting->mName == "Saturation") {
                    sat = static_cast<NumberSetting*>(setting)->mValue;
                } else if (setting->mName == "Colors") {
                    numColors = (int)static_cast<NumberSetting*>(setting)->mValue;
                }
            }

            ImColor base(255, 255, 255);
            // Themes. Index order IS the enum order in ClickGUI's "Theme" setting
            // and is persisted by ConfigManager as an int, so inserting a theme in
            // the middle silently re-themes every existing config — append only.
            //   0 Aurora        teal -> indigo -> violet, the default
            //   1 Ember         amber -> crimson
            //   2 Frost         ice blue -> white -> steel
            //   3 Neon          magenta -> cyan, high chroma
            //   4 Mint          spring green -> aqua
            //   5 Void          deep indigo -> plum, low luminance
            //   6 Sakura        blush pink -> pale gold
            //   7 Spectrum      full-hue sweep (was "Rainbow")
            //   8 Custom        user palette
            //   9..16 Rise-style themes (Fire, Water, Smoke, Sunset, Cotton
            //             Candy, Forest, Ocean, Purple Haze) - appended after
            //             Custom so persisted indices and the == 8 Custom
            //             checks keep their meaning.
            //   17..22 Appended later (Lemonade, Copper, Abyss, Orchid,
            //             Safari, Cobalt) - same rule: append only.
            if (themeIdx == 0) { // Aurora
                std::vector<ImColor> colors = {
                    ImColor(58, 226, 199),
                    ImColor(72, 149, 239),
                    ImColor(129, 96, 240),
                    ImColor(72, 149, 239)
                };
                base = LerpColors(speed, index, colors, ms);
            } else if (themeIdx == 1) { // Ember
                std::vector<ImColor> colors = {
                    ImColor(255, 196, 84),
                    ImColor(255, 122, 48),
                    ImColor(232, 58, 78),
                    ImColor(255, 122, 48)
                };
                base = LerpColors(speed, index, colors, ms);
            } else if (themeIdx == 2) { // Frost
                std::vector<ImColor> colors = {
                    ImColor(140, 214, 255),
                    ImColor(226, 244, 255),
                    ImColor(126, 162, 208),
                    ImColor(180, 226, 255)
                };
                base = LerpColors(speed, index, colors, ms);
            } else if (themeIdx == 3) { // Neon
                std::vector<ImColor> colors = {
                    ImColor(255, 46, 166),
                    ImColor(168, 62, 255),
                    ImColor(46, 226, 255),
                    ImColor(168, 62, 255)
                };
                base = LerpColors(speed, index, colors, ms);
            } else if (themeIdx == 4) { // Mint
                std::vector<ImColor> colors = {
                    ImColor(120, 245, 168),
                    ImColor(64, 224, 208),
                    ImColor(150, 250, 220),
                    ImColor(64, 224, 208)
                };
                base = LerpColors(speed, index, colors, ms);
            } else if (themeIdx == 5) { // Void
                std::vector<ImColor> colors = {
                    ImColor(94, 84, 214),
                    ImColor(158, 88, 214),
                    ImColor(196, 108, 196),
                    ImColor(112, 76, 196)
                };
                base = LerpColors(speed, index, colors, ms);
            } else if (themeIdx == 6) { // Sakura
                std::vector<ImColor> colors = {
                    ImColor(255, 168, 200),
                    ImColor(255, 214, 226),
                    ImColor(250, 226, 170),
                    ImColor(255, 190, 210)
                };
                base = LerpColors(speed, index, colors, ms);
            } else if (themeIdx == 7) { // Spectrum
                base = Rainbow(speed, sat, 1.0f, (int)index);
            } else if (themeIdx == 8) { // Custom
                for (int c = 1; c <= numColors; c++) {
                    std::string cName = "Color " + std::to_string(c);
                    for (auto* setting : iface->mSettings) {
                        if (!setting) continue;
                        if (setting->mName == cName) {
                            auto* cSet = static_cast<ColorSetting*>(setting);
                            customColors.push_back(ImColor(cSet->mValue[0], cSet->mValue[1], cSet->mValue[2], cSet->mValue[3]));
                        }
                    }
                }
                base = LerpColors(speed, index, customColors, ms);
            } else if (themeIdx == 9) { // Fire
                std::vector<ImColor> colors = {
                    ImColor(255, 102, 0),
                    ImColor(255, 196, 0),
                    ImColor(232, 58, 44),
                    ImColor(255, 102, 0)
                };
                base = LerpColors(speed, index, colors, ms);
            } else if (themeIdx == 10) { // Water
                std::vector<ImColor> colors = {
                    ImColor(60, 150, 255),
                    ImColor(80, 220, 255),
                    ImColor(52, 90, 200),
                    ImColor(60, 150, 255)
                };
                base = LerpColors(speed, index, colors, ms);
            } else if (themeIdx == 11) { // Smoke
                std::vector<ImColor> colors = {
                    ImColor(120, 124, 140),
                    ImColor(170, 174, 190),
                    ImColor(90, 92, 105),
                    ImColor(120, 124, 140)
                };
                base = LerpColors(speed, index, colors, ms);
            } else if (themeIdx == 12) { // Sunset
                std::vector<ImColor> colors = {
                    ImColor(255, 120, 80),
                    ImColor(255, 80, 160),
                    ImColor(120, 80, 255),
                    ImColor(255, 120, 80)
                };
                base = LerpColors(speed, index, colors, ms);
            } else if (themeIdx == 13) { // Cotton Candy
                std::vector<ImColor> colors = {
                    ImColor(255, 150, 220),
                    ImColor(150, 200, 255),
                    ImColor(200, 120, 255),
                    ImColor(255, 150, 220)
                };
                base = LerpColors(speed, index, colors, ms);
            } else if (themeIdx == 14) { // Forest
                std::vector<ImColor> colors = {
                    ImColor(60, 180, 90),
                    ImColor(120, 200, 80),
                    ImColor(40, 120, 70),
                    ImColor(60, 180, 90)
                };
                base = LerpColors(speed, index, colors, ms);
            } else if (themeIdx == 15) { // Ocean
                std::vector<ImColor> colors = {
                    ImColor(0, 180, 220),
                    ImColor(0, 120, 200),
                    ImColor(60, 220, 240),
                    ImColor(0, 180, 220)
                };
                base = LerpColors(speed, index, colors, ms);
            } else if (themeIdx == 16) { // Purple Haze
                std::vector<ImColor> colors = {
                    ImColor(150, 80, 255),
                    ImColor(100, 60, 200),
                    ImColor(200, 140, 255),
                    ImColor(150, 80, 255)
                };
                base = LerpColors(speed, index, colors, ms);
            } else if (themeIdx == 17) { // Lemonade
                std::vector<ImColor> colors = {
                    ImColor(255, 236, 128),
                    ImColor(198, 233, 92),
                    ImColor(124, 199, 120),
                    ImColor(255, 236, 128)
                };
                base = LerpColors(speed, index, colors, ms);
            } else if (themeIdx == 18) { // Copper
                std::vector<ImColor> colors = {
                    ImColor(217, 140, 79),
                    ImColor(241, 193, 124),
                    ImColor(172, 86, 62),
                    ImColor(217, 140, 79)
                };
                base = LerpColors(speed, index, colors, ms);
            } else if (themeIdx == 19) { // Abyss
                std::vector<ImColor> colors = {
                    ImColor(38, 64, 124),
                    ImColor(26, 101, 133),
                    ImColor(19, 42, 92),
                    ImColor(38, 64, 124)
                };
                base = LerpColors(speed, index, colors, ms);
            } else if (themeIdx == 20) { // Orchid
                std::vector<ImColor> colors = {
                    ImColor(191, 132, 222),
                    ImColor(224, 124, 192),
                    ImColor(242, 192, 222),
                    ImColor(191, 132, 222)
                };
                base = LerpColors(speed, index, colors, ms);
            } else if (themeIdx == 21) { // Safari
                std::vector<ImColor> colors = {
                    ImColor(224, 186, 138),
                    ImColor(156, 172, 122),
                    ImColor(198, 150, 98),
                    ImColor(224, 186, 138)
                };
                base = LerpColors(speed, index, colors, ms);
            } else if (themeIdx == 22) { // Cobalt
                std::vector<ImColor> colors = {
                    ImColor(40, 90, 220),
                    ImColor(70, 140, 255),
                    ImColor(140, 170, 255),
                    ImColor(40, 90, 220)
                };
                base = LerpColors(speed, index, colors, ms);
            } else {
                base = LerpColors(speed, index, { ImColor(58, 226, 199), ImColor(129, 96, 240) }, ms);
            }
            return applyGradientStyle(base, index, gradStyle, speed, ms);
        }

        // Fallback used before ModuleManager has built the module list.
        std::vector<ImColor> theme = {
            ImColor(58, 226, 199),
            ImColor(72, 149, 239),
            ImColor(129, 96, 240),
            ImColor(72, 149, 239)
        };
        return LerpColors(2.0f, index, theme, ms);
    }
}