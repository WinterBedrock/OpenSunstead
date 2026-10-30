#pragma once
// Shared renderer for the counter-style HUD modules (FPS / Ping / BPS /
// Coordinates / CPS) — the Rise counter look: a frosted plate (backdrop
// blur + light dim), the VALUE in the theme's live gradient, the SUFFIX in
// plain text colour, "120 FPS" / "45 ms" — never "FPS: 120".
//
// The caller owns the settings (added by name, so ctor order is free):
//   "Text Color"         ColorSetting   suffix / plain-value colour
//   "Background Blur"    NumberSetting  backdrop blur strength
//   "Background Dim"     NumberSetting  plate alpha
//   "Background Rounding" NumberSetting plate corner radius
//   "Text Shadow"        BoolSetting    drop shadow behind the text
//   "Shadow Opacity"     NumberSetting  plate drop-shadow alpha
//   "Gradient Numbers"   BoolSetting    value drawn in the theme gradient
#include "../Module.h"
#include "../Setting.h"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../../Utils/MiscUtils/D2D.hpp"
#include <imgui.h>
#include <cmath>
#include <string>
#include <unordered_map>
#include <algorithm>

// value = the number/coordinates string, suffix = "FPS" / "ms" / ... (may be
// empty). valueOverride = optional RGB the value takes when Gradient Numbers
// is off. fontSize/baseHeight keep each module's historical metrics.
inline void DrawCounterModule(Module* self, const std::string& value, const std::string& suffix,
                              float fontSize = 22.0f, const float* valueOverride = nullptr) {
    Setting* set[8] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    static const char* kNames[8] = { "Text Color", "Background Blur", "Background Dim",
                                     "Background Rounding", "Text Shadow", "Shadow Opacity",
                                     "Gradient Numbers", "Number Animation" };
    for (Setting* s : self->mSettings)
        for (int i = 0; i < 8; ++i)
            if (s && s->mName == kNames[i] && !set[i]) set[i] = s;

    ColorSetting*  textColor   = static_cast<ColorSetting*>(set[0]);
    NumberSetting* bgBlur      = static_cast<NumberSetting*>(set[1]);
    NumberSetting* bgDim       = static_cast<NumberSetting*>(set[2]);
    NumberSetting* bgRounding  = static_cast<NumberSetting*>(set[3]);
    BoolSetting*   textShadow  = static_cast<BoolSetting*>(set[4]);
    NumberSetting* shadowAlpha = static_cast<NumberSetting*>(set[5]);
    BoolSetting*   gradientNum = static_cast<BoolSetting*>(set[6]);
    BoolSetting*   numAnim     = static_cast<BoolSetting*>(set[7]);

    // ---- value-change animation (Rise-style rolling readout) --------------
    // When the displayed value changes, the old number slides up and fades
    // while the new one slides in from below — a smooth roll instead of a
    // hard digit snap. Per-module state keyed by the Module pointer.
    struct NumAnim { std::string last; std::string prev; float t = 1.0f; };
    static std::unordered_map<void*, NumAnim> s_anim;
    NumAnim& anim = s_anim[self];
    const bool doAnim = numAnim && numAnim->mValue;
    if (value != anim.last) {
        anim.prev = anim.last.empty() ? value : anim.last;
        anim.last = value;
        anim.t = doAnim ? 0.0f : 1.0f;
    }
    if (!doAnim) anim.t = 1.0f;
    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f) dt = 1.0f / 60.0f;
    anim.t += (1.0f - anim.t) * std::clamp(dt * 10.0f, 0.0f, 1.0f);
    if (anim.t > 0.998f) anim.t = 1.0f;
    const bool animating = anim.t < 1.0f;
    const std::string& shownValue = animating ? anim.last : value;
    // During the roll both the outgoing and the incoming numbers are on screen;
    // measure the wider one so the plate never squeezes.
    const std::string& measureValue = animating
        ? (ImRenderUtils::getTextWidth(&anim.prev, fontSize * self->mScale) >
           ImRenderUtils::getTextWidth(&anim.last, fontSize * self->mScale) ? anim.prev : anim.last)
        : shownValue;

    const float scale = self->mScale;
    const float fs = fontSize * scale;
    const float gap = 6.0f * scale;
    const float valueW = measureValue.empty() ? 0.0f : ImRenderUtils::getTextWidth(&measureValue, fs);
    const float suffixW = suffix.empty() ? 0.0f : ImRenderUtils::getTextWidth(&suffix, fs);
    const float textW = valueW + (suffix.empty() ? 0.0f : gap + suffixW);
    const float width = textW + 20.0f * scale;
    const float height = fontSize * scale + 9.0f * scale;

    self->mWidth = width;
    self->mHeight = height;

    const ImVec4 rect(self->mPosX, self->mPosY, self->mPosX + width, self->mPosY + height);
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();

    // ---- frosted plate ------------------------------------------------------
    const float blur = bgBlur ? bgBlur->mValue : 0.0f;
    const float dim = bgDim ? bgDim->mValue : 0.30f;
    const float rounding = (bgRounding ? bgRounding->mValue : 5.0f) * scale;
    if (blur > 0.01f)
        D2D::addBlurOptimized(drawList, blur, std::optional<ImVec4>(rect), rounding);
    const float shadowA = shadowAlpha ? shadowAlpha->mValue : 0.35f;
    if (shadowA > 0.01f)
        ImRenderUtils::fillShadowRectangle(rect, ImColor(0, 0, 0), shadowA, rounding, 12.0f * scale);
    ImRenderUtils::fillRectangle(rect, ImColor(12, 12, 15, (int)(255 * dim)), 1.0f, rounding);
    drawList->AddRect(ImVec2(rect.x, rect.y), ImVec2(rect.z, rect.w),
                      ImColor(255, 255, 255, 25), rounding, 0, 1.0f);

    // ---- "[gradient value] SUFFIX" ------------------------------------------
    const bool doShadow = textShadow && textShadow->mValue;
    const uint64_t nowMs = ColorUtils::GetTimeMs();
    const ImVec2 base(rect.x + 10.0f * scale, rect.y + 4.0f * scale);
    const ImColor suffixCol = textColor
        ? ImColor(textColor->mValue[0], textColor->mValue[1], textColor->mValue[2], textColor->mValue[3])
        : ImColor(1.0f, 1.0f, 1.0f, 1.0f);
    const bool gradient = !gradientNum || gradientNum->mValue;

    if (doShadow) {
        // One shadow pass under everything keeps the glyphs crisp.
        if (!value.empty())
            ImRenderUtils::drawText(ImVec2(base.x + 1.0f, base.y + 1.0f), value,
                                    ImColor(0, 0, 0), fs, 0.6f, false, drawList);
        if (!suffix.empty())
            ImRenderUtils::drawText(ImVec2(base.x + valueW + gap + 1.0f, base.y + 1.0f), suffix,
                                    ImColor(0, 0, 0), fs, 0.6f, false, drawList);
    }

    float x = base.x;
    const auto drawValue = [&](const std::string& text, float aMul, float yOff) {
        if (text.empty() || aMul <= 0.01f) return;
        if (gradient) {
            // Per-digit theme colour, sweeping along the palette like the
            // ArrayList gradient does down its rows.
            float idx = 0.0f;
            float vx = x;
            for (char c : text) {
                const std::string glyph(1, c);
                const ImColor col = ColorUtils::getThemedColor(idx, nowMs);
                ImRenderUtils::drawText(ImVec2(vx, base.y + yOff), glyph, col, fs, aMul, false, drawList);
                vx += ImRenderUtils::getTextWidth(&glyph, fs);
                idx += 3.0f;
            }
        } else {
            const ImColor col = valueOverride
                ? ImColor(valueOverride[0], valueOverride[1], valueOverride[2], 1.0f)
                : suffixCol;
            ImRenderUtils::drawText(ImVec2(x, base.y + yOff), text, col, fs, aMul, false, drawList);
        }
    };

    if (animating) {
        // Per-digit odometer: digits that did not change stay put; only the
        // changed ones roll (old glyph slides up and fades, new one slides in
        // from below). Positions align from the RIGHT, so "51" -> "56" rolls
        // just the trailing digit and "51" -> "60" rolls both.
        const int newLen = (int)value.size();
        const int oldLen = (int)anim.prev.size();
        const float roll = fs * 0.45f;
        float dx_ = x;
        float gradIdx = 0.0f;
        for (int i = 0; i < newLen; ++i) {
            const std::string glyph(1, value[i]);
            const float gw = ImRenderUtils::getTextWidth(&glyph, fs);
            const int oldIdx = i - (newLen - oldLen);   // right-aligned comparison
            const bool unchanged = (oldIdx >= 0 && oldIdx < oldLen &&
                                   anim.prev[oldIdx] == value[i]);
            if (unchanged) {
                if (gradient) {
                    ImRenderUtils::drawText(ImVec2(dx_, base.y), glyph,
                                             ColorUtils::getThemedColor(gradIdx, nowMs), fs, 1.0f, false, drawList);
                } else {
                    ImRenderUtils::drawText(ImVec2(dx_, base.y), glyph, suffixCol, fs, 1.0f, false, drawList);
                }
            } else {
                // The rolling digit: outgoing + incoming glyph share the cell.
                const std::string oldGlyph = (oldIdx >= 0 && oldIdx < oldLen)
                    ? std::string(1, anim.prev[oldIdx]) : std::string();
                if (gradient) {
                    if (!oldGlyph.empty())
                        ImRenderUtils::drawText(ImVec2(dx_, base.y - anim.t * roll), oldGlyph,
                                                ColorUtils::getThemedColor(gradIdx, nowMs), fs, 1.0f - anim.t, false, drawList);
                    ImRenderUtils::drawText(ImVec2(dx_, base.y + (1.0f - anim.t) * roll), glyph,
                                            ColorUtils::getThemedColor(gradIdx, nowMs), fs, anim.t, false, drawList);
                } else {
                    const ImColor cellCol = valueOverride
                        ? ImColor(valueOverride[0], valueOverride[1], valueOverride[2], 1.0f)
                        : suffixCol;
                    if (!oldGlyph.empty())
                        ImRenderUtils::drawText(ImVec2(dx_, base.y - anim.t * roll), oldGlyph, cellCol, fs, 1.0f - anim.t, false, drawList);
                    ImRenderUtils::drawText(ImVec2(dx_, base.y + (1.0f - anim.t) * roll), glyph, cellCol, fs, anim.t, false, drawList);
                }
            }
            dx_ += gw;
            gradIdx += 3.0f;
        }
    } else {
        drawValue(value, 1.0f, 0.0f);
    }
    x += valueW;
    if (!suffix.empty())
        ImRenderUtils::drawText(ImVec2(x + gap, base.y), suffix, suffixCol, fs, 1.0f, false, drawList);
}
