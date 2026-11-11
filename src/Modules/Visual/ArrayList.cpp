#include "ArrayList.h"

#include "../ModuleManager.h"
#include "Notifications.h"
#include "../Misc/ToggleSounds.h"
#include "../../Utils/MiscUtils/ColorUtils.hpp"
#include "../../Utils/MiscUtils/D2D.hpp"
#include "../../Utils/MiscUtils/ImRenderUtils.hpp"
#include "../../Utils/MiscUtils/MathUtils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <unordered_map>

namespace {

bool isHiddenModule(const std::string& name) {
    return name == "ClickGUI";
}

std::string modeLabelOf(Module* mod) {
    for (Setting* s : mod->mSettings) {
        if (!s || s->mType != SettingType::Enum) continue;
        auto* e = static_cast<EnumSetting*>(s);
        if (e->mValue >= 0 && e->mValue < static_cast<int>(e->mValues.size()))
            return e->mValues[e->mValue];
    }
    return {};
}

float& hoverAmountFor(Module* mod) {
    static std::unordered_map<Module*, float> s_hover;
    return s_hover[mod];
}

float easeOutCubic(float t) {
    t = std::clamp(t, 0.f, 1.f);
    const float u = 1.f - t;
    return 1.f - u * u * u;
}

float easeOutBack(float t) {
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;
    constexpr float c1 = 1.70158f;
    constexpr float c3 = c1 + 1.f;
    const float u = t - 1.f;
    return 1.f + c3 * u * u * u + c1 * u * u;
}

constexpr float kRiseTagGap  = 3.f;  // gap between name and tag (Rise: +3.0)
constexpr float kEdgeMargin  = 8.f;  // px inset from the screen edge
constexpr float kVisibleAnim = 0.01f;
constexpr float kSettledAnim = 0.002f;

} // namespace

ArrayList::ArrayList()
    : Module("ArrayList", "Displays enabled modules", Category::Visual, 0) {
    mIsHudModule = true;
    mPosX = -1.f;
    mPosY = -1.f;

    mCorner  = add<EnumSetting>("Mode", "Screen corner the list anchors to", 1,
                                std::vector<std::string>{ "Top Left", "Top Right", "Bottom Left", "Bottom Right" });
    mCasing  = add<EnumSetting>("Casing", "Module name casing", 0,
                                std::vector<std::string>{ "Default", "lowercase" });
    if (mCasing->mValue >= (int)mCasing->mValues.size()) mCasing->mValue = 0;

    mColorMode    = add<EnumSetting>("Color Mode", "Where the text gradient comes from", 0,
                                     std::vector<std::string>{ "Themed", "Custom" });
    mCustomColor  = add<ColorSetting>("Custom Color", "Gradient start for Custom mode", 0.42f, 0.58f, 1.f, 1.f);
    mCustomColor2 = add<ColorSetting>("Custom Color 2", "Gradient end for Custom mode", 0.72f, 0.44f, 1.f, 1.f);
    mGradientSpan = add<NumberSetting>("Gradient Span", "How far the gradient travels down the list", 1.f, 0.f, 3.f, 0.05f);

    mTextShadow   = add<BoolSetting>("Text Shadow", "Drop shadow behind the module names", false);
    mShowMode     = add<BoolSetting>("Render Mode", "Append the module's selected mode", true);
    mModeDash     = add<BoolSetting>("Mode Dash", "Prefix the mode label with a dash", true);
    mModeThemed   = add<BoolSetting>("Mode Theme Color", "Tint the mode text with the accent", false);
    mModeColor    = add<ColorSetting>("Mode Color", "Mode text colour", 0.80f, 0.80f, 0.80f, 1.f);

    mBackground    = add<BoolSetting>("Render BG", "Draw a panel behind each row", true);
    mBgOpacity     = add<NumberSetting>("Background Opacity", "Panel alpha", 0.58f, 0.f, 1.f, 0.05f);
    mBlur          = add<NumberSetting>("Blur Strength", "Backdrop blur behind the list", 0.f, 0.f, 20.f, 0.5f);
    mShadow        = add<BoolSetting>("Shadow", "Rise \"Shaders\": frosted backdrop + dimmed plates", true);
    mShadowOpacity = add<NumberSetting>("Shadow Opacity", "Per-row drop shadow strength", 0.55f, 0.f, 1.f, 0.05f);
    mGlow          = add<BoolSetting>("Glow", "Accent glow around the sidebar bar only", true);
    mGlowStrength  = add<NumberSetting>("Glow Strength", "Sidebar bar glow intensity", 0.7f, 0.1f, 1.5f, 0.05f);

    mBarWidth   = add<NumberSetting>("Bar Width", "Sidebar bar thickness (px)", 4.f, 1.f, 14.f, 0.5f);
    mBarHeight  = add<NumberSetting>("Bar Height", "Sidebar bar height (% of row)", 80.f, 30.f, 100.f, 1.f);
    mBarRadius  = add<NumberSetting>("Bar Radius", "Sidebar bar corner rounding (px)", 2.f, 0.f, 7.f, 0.5f);
    mBarPadding = add<NumberSetting>("Bar Padding", "Gap between the text and the sidebar bar (px)", 0.f, 0.f, 24.f, 0.5f);

    mFontSize  = add<NumberSetting>("Font Size", "Text size", 17.f, 10.f, 32.f, 0.5f);
    mCardWidth  = add<NumberSetting>("Card Width", "Card padding left/right of the module text", 4.f, 0.f, 20.f, 0.5f);
    mCardHeight = add<NumberSetting>("Card Height", "Card padding above/below the module text", 1.f, 0.f, 14.f, 0.5f);
    mOffsetX  = add<NumberSetting>("H Offset", "Extra inset from the screen edge", 0.f, 0.f, 40.f, 1.f);
    mOffsetY  = add<NumberSetting>("V Offset", "Extra inset from the screen edge", 0.f, 0.f, 40.f, 1.f);

    mAnimMode    = add<EnumSetting>("Animation", "How rows enter and leave", 0,
                                    std::vector<std::string>{ "Slide", "Fade", "Scale", "Swipe" });
    mAnimSpeed   = add<NumberSetting>("Anim Speed", "Enter / leave speed", 12.f, 4.f, 24.f, 0.5f);
    mMouseToggle = add<BoolSetting>("Mouse Toggle", "Hover highlights a row, click disables it", true);

    UpdateSettingsVisibility();
}

void ArrayList::UpdateSettingsVisibility() {
    const bool custom = (mColorMode->mValue == 1);
    mCustomColor->mVisible  = custom;
    mCustomColor2->mVisible = custom;

    mModeDash->mVisible     = mShowMode->mValue;
    mModeThemed->mVisible   = mShowMode->mValue;
    mModeColor->mVisible    = mShowMode->mValue && !mModeThemed->mValue;

    mBgOpacity->mVisible     = mBackground->mValue;
    mShadowOpacity->mVisible = mShadow->mValue;
    mGlowStrength->mVisible  = mGlow->mValue;
}

ArrayList::Frame ArrayList::buildFrame() const {
    Frame f;
    f.fontSize  = mFontSize->mValue * mScale;
    f.padX      = mCardWidth->mValue * mScale;
    f.padY      = mCardHeight->mValue * mScale;
    f.rowHeight = f.fontSize + f.padY * 2.f;
    f.gap       = 0.f;   // rows stack CONTIGUOUSLY, plates touching (Rise)
    f.glow      = mGlow->mValue ? mGlowStrength->mValue : 0.f;
    f.barW      = mBarWidth->mValue;
    f.barHeight = mBarHeight->mValue;
    f.barR      = mBarRadius->mValue;
    f.barPad    = mBarPadding->mValue;
    f.corner    = corner();
    f.anim      = animMode();
    f.right     = (f.corner == Corner::TopRight || f.corner == Corner::BottomRight);
    f.bottom    = (f.corner == Corner::BottomLeft || f.corner == Corner::BottomRight);

    return f;
}

ImColor ArrayList::colorAt(float index) const {
    const float span = mGradientSpan->mValue;
    if (mColorMode->mValue != 1) return ColorUtils::getThemedColor(index * span);

    const float t = 0.5f + 0.5f * std::sin(index * span * 0.05f);
    const float* a = mCustomColor->mValue;
    const float* b = mCustomColor2->mValue;
    return ImColor(a[0] + (b[0] - a[0]) * t,
                   a[1] + (b[1] - a[1]) * t,
                   a[2] + (b[2] - a[2]) * t, 1.f);
}

std::string ArrayList::applyCasing(std::string text) const {
    switch (casing()) {
    case Casing::Lower:
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        break;

    case Casing::Keep:
        break;
    }
    return text;
}

std::vector<ArrayList::Entry> ArrayList::collect(const Frame& f, float dt) const {
    const float speed = mAnimSpeed->mValue;
    std::vector<Entry> entries;

    for (Module* mod : ModuleManager::GetInstance().GetModules()) {
        if (!mod || isHiddenModule(mod->GetName())) continue;
        if (!mod->IsVisibleInArrayList()) continue;

        const float target = mod->IsEnabled() ? 1.f : 0.f;
        mod->arrayListAnim = MathUtils::animate(target, mod->arrayListAnim,
                                                std::clamp(dt * speed, 0.f, 1.f));
        if (std::fabs(mod->arrayListAnim - target) < kSettledAnim) mod->arrayListAnim = target;
        if (mod->arrayListAnim <= kVisibleAnim) {
            mod->arrayListY = -1.f; // fully gone: next appearance snaps in place
            continue;
        }

        Entry e;
        e.mod  = mod;
        e.anim = std::min(mod->arrayListAnim, 1.f);
        e.name = applyCasing(mod->GetName());
        if (mShowMode->mValue) {
            std::string mode = applyCasing(modeLabelOf(mod));
            if (!mode.empty() && mModeDash->mValue) mode = "- " + mode;
            e.mode = std::move(mode);
        }

        e.nameWidth = ImRenderUtils::getTextWidth(&e.name, f.fontSize);
        // Rise puts the tag 3px past the name (arrayListFont.a(tag, x + nameW + 3)).
        e.modeWidth = e.mode.empty()
            ? 0.f
            : ImRenderUtils::getTextWidth(&e.mode, f.fontSize) + kRiseTagGap * mScale;
        e.width = e.nameWidth + e.modeWidth + f.padX * 2.f;
        entries.push_back(std::move(e));
    }

    const bool widestFirst = !f.bottom;
    std::sort(entries.begin(), entries.end(),
              [widestFirst](const Entry& a, const Entry& b) {
                  return widestFirst ? (a.width > b.width) : (a.width < b.width);
              });
    return entries;
}

std::vector<ArrayList::Slot> ArrayList::layout(const Frame& f, const std::vector<Entry>& entries,
                                               float originX, float originY, float dt,
                                               ImVec4* boundsOut) const {
    std::vector<Slot> slots;
    slots.reserve(entries.size());

    float left = FLT_MAX, top = FLT_MAX, right = -FLT_MAX, bottom = -FLT_MAX;
    float stackY = originY;

    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        const float ease = easeOutBack(e.anim);
        const float height = f.rowHeight * e.anim;

        if (e.mod->arrayListY < 0.f) e.mod->arrayListY = stackY;
        else e.mod->arrayListY = MathUtils::animate(stackY, e.mod->arrayListY,
                                                    std::clamp(dt * 15.f, 0.f, 1.f));
        if (std::fabs(e.mod->arrayListY - stackY) < 0.25f) e.mod->arrayListY = stackY;
        const float y = e.mod->arrayListY;

        float offset = 0.f;
        float width = e.width;
        switch (f.anim) {
        case Anim::Slide: offset = (1.f - ease) * (e.width + 16.f * mScale); break;
        case Anim::Fade:  break;
        case Anim::Scale: width = e.width * std::max(ease, 0.f); break;
        case Anim::Swipe: offset = (1.f - easeOutCubic(e.anim)) * (e.width + 60.f * mScale); break;
        }

        Slot slot;
        if (f.right) {
            slot.right = originX + offset;
            slot.left  = slot.right - width;
        } else {
            slot.left  = originX - offset;
            slot.right = slot.left + width;
        }
        slot.top    = y;
        slot.bottom = y + height;
        slot.anim   = e.anim;
        slot.ease   = std::clamp(ease, 0.f, 1.f);

        if (e.anim > 0.05f) {
            left   = std::min(left, slot.left);
            top    = std::min(top, slot.top);
            right  = std::max(right, slot.right);
            bottom = std::max(bottom, slot.bottom);
        }

        slots.push_back(slot);
        stackY += height;
        if (i + 1 < entries.size()) stackY += f.gap * e.anim;
    }

    if (boundsOut) *boundsOut = ImVec4(left, top, right, bottom);
    return slots;
}

void ArrayList::paintRowShadow(const Frame& f, const Slot& slot, ImDrawList* dl) const {
    const float w = slot.right - slot.left;
    const float h = slot.bottom - slot.top;
    if (slot.ease <= 0.05f || w < 0.5f || h < 0.5f) return;

    if (mShadow->mValue && mShadowOpacity->mValue > 0.01f) {
        const float a = mShadowOpacity->mValue * slot.ease;
        if (a > 0.01f)
            ImRenderUtils::fillShadowRectSoft(
                ImVec4(slot.left - 3.f * mScale, slot.top,
                       slot.right + 3.f * mScale, slot.bottom),
                ImColor(0.f, 0.f, 0.f, 1.f), a, 200.f * mScale, 0.f, dl);
    }
}

void ArrayList::paintSurface(const Frame& f, const Slot& slot, float hover,
                             ImDrawList* dl) const {
    const float w = slot.right - slot.left;
    const float h = slot.bottom - slot.top;
    if (w < 0.5f || h < 0.5f) return;

    if (mBackground->mValue) {
        const float shaderDim = mShadow->mValue ? 0.75f : 1.0f;
        const float alpha = mBgOpacity->mValue * shaderDim * slot.ease;
        dl->AddRectFilled(ImVec2(slot.left, slot.top), ImVec2(slot.right, slot.bottom),
                          ImGui::GetColorU32(ImVec4(0.f, 0.f, 0.f, alpha)));
    }
    if (hover > 0.01f) {
        dl->AddRectFilled(ImVec2(slot.left, slot.top), ImVec2(slot.right, slot.bottom),
                          ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, 0.07f * hover * slot.ease)));
    }
}

void ArrayList::paintText(const Frame& f, const Entry& entry, const Slot& slot,
                          const ImColor& accent, float /*gradientBase*/, ImDrawList* dl) const {
    if (slot.anim <= 0.05f) return;
    if (slot.right - slot.left < 0.5f) return;

    const float barW   = f.barW * mScale;
    const float barPad = f.barPad * mScale;
    const float textX  = slot.left + f.padX;
    const float textY  = slot.top + f.padY;

    const ImVec4 rgb = accent.Value;
    const float textA = slot.ease;
    const ImColor nameCol(rgb.x, rgb.y, rgb.z, textA);
    const bool shadow = mTextShadow->mValue;
    const float fs = f.fontSize * ImRenderUtils::g_visualFontScale;
    if (shadow && textA > 0.05f) {
        dl->AddText(ImGui::GetFont(), fs, ImVec2(textX + 1.0f * mScale, textY + 1.0f * mScale),
                    ImGui::GetColorU32(ImVec4(0.f, 0.f, 0.f, 0.55f * textA)), entry.name.c_str());
    }
    dl->AddText(ImGui::GetFont(), fs, ImVec2(textX, textY), nameCol, entry.name.c_str());
    if (!entry.mode.empty()) {
        const ImColor tagCol = mModeThemed->mValue
            ? ImColor(rgb.x, rgb.y, rgb.z, textA)
            : ImColor(mModeColor->mValue[0], mModeColor->mValue[1], mModeColor->mValue[2], textA);
        if (shadow && textA > 0.05f) {
            dl->AddText(ImGui::GetFont(), fs,
                        ImVec2(textX + entry.nameWidth + kRiseTagGap * mScale + 1.0f * mScale,
                               textY + 1.0f * mScale),
                        ImGui::GetColorU32(ImVec4(0.f, 0.f, 0.f, 0.55f * textA)),
                        entry.mode.c_str());
        }
        dl->AddText(ImGui::GetFont(), fs,
                    ImVec2(textX + entry.nameWidth + kRiseTagGap * mScale, textY),
                    tagCol, entry.mode.c_str());
    }
    const float rowH   = slot.bottom - slot.top;
    const float barH   = rowH * (f.barHeight / 100.f);
    float barX0, barX1;
    if (f.right) { barX1 = slot.right + barPad + barW; barX0 = barX1 - barW; }
    else         { barX0 = slot.left  - barPad - barW; barX1 = barX0 + barW; }
    const float barY   = slot.top + (rowH - barH) * 0.5f;
    const float radius = std::clamp(f.barR * mScale, 0.f, std::min(barW, barH) * 0.5f);
    const ImDrawFlags roundFlags = f.right ? ImDrawFlags_RoundCornersRight
                                           : ImDrawFlags_RoundCornersLeft;
    if (f.glow > 0.01f && slot.ease > 0.05f) {
        ImRenderUtils::fillShadowRectangle(ImVec4(barX0, barY, barX1, barY + barH), accent,
                                          0.5f * f.glow * slot.ease, radius,
                                          8.f * f.glow * mScale, 1.0f, dl);
    }
    dl->AddRectFilled(ImVec2(barX0, barY), ImVec2(barX1, barY + barH),
                      ImGui::GetColorU32(ImVec4(rgb.x, rgb.y, rgb.z, slot.ease)),
                      radius, roundFlags);
}

void ArrayList::OnRender() {
    if (!IsEnabled()) return;

    UpdateSettingsVisibility();

    const Frame f = buildFrame();
    const float dt = ImRenderUtils::getDeltaTime();

    std::vector<Entry> entries = collect(f, dt);
    if (entries.empty()) {
        mWidth = 0.f;
        mHeight = 0.f;
        return;
    }

    float widest = 0.f;
    float totalHeight = 0.f;
    for (size_t i = 0; i < entries.size(); ++i) {
        widest = std::max(widest, entries[i].width);
        totalHeight += f.rowHeight * entries[i].anim;
        if (i + 1 < entries.size()) totalHeight += f.gap * entries[i].anim;
    }
    mWidth = widest;
    mHeight = totalHeight;

    const ImVec2 screen = ImRenderUtils::getScreenSize();
    const float marginX = (kEdgeMargin + mOffsetX->mValue) * mScale;
    const float marginY = (kEdgeMargin + mOffsetY->mValue) * mScale;
    const float barRoom = (f.barPad + f.barW) * mScale;
    const float originX = f.right ? (screen.x - marginX - barRoom) : (marginX + barRoom);
    const float originY = f.bottom ? (screen.y - marginY - totalHeight) : marginY;

    mPosX = f.right ? (originX - widest) : originX;
    mPosY = originY;

    ImVec4 bounds;
    const std::vector<Slot> slots = layout(f, entries, originX, originY, dt, &bounds);


    bool guiOpen = false;
    if (Module* gui = ModuleManager::GetInstance().GetModuleByName("ClickGUI"))
        guiOpen = gui->IsEnabled();

    const bool mouseEnabled = mMouseToggle->mValue;
    const bool clicked = mouseEnabled && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

    float riseBlur = (mBlur->mValue > 0.1f) ? mBlur->mValue : (mShadow->mValue ? 8.f : 0.f);
    const bool blurRows = !guiOpen && mBackground->mValue && riseBlur > 0.1f;
    if (blurRows) {
        ImDrawList* bgList = ImGui::GetBackgroundDrawList();
        for (const Slot& slot : slots) {
            const float w = slot.right - slot.left;
            const float h = slot.bottom - slot.top;
            if (slot.anim <= 0.05f || w < 1.f || h < 1.f) continue;
            D2D::addBlurOptimized(bgList, riseBlur,
                                  std::optional<ImVec4>(ImVec4(slot.left, slot.top,
                                                               slot.right, slot.bottom)),
                                  0.f);
        }
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    {
        for (size_t i = 0; i < entries.size(); ++i) {
            const Slot& slot = slots[i];
            if (slot.anim <= 0.05f) continue;
            paintRowShadow(f, slot, dl);
        }
    }

    // Walking the gradient index down the list is what makes the colour flow
    // continue across rows instead of restarting on each one.
    float gradientBase = 0.f;
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        const Slot& slot = slots[i];

        const bool hovered = mouseEnabled && slot.anim > 0.95f &&
                             ImRenderUtils::isMouseOver(ImVec4(slot.left, slot.top,
                                                               slot.right, slot.bottom));

        // Ease the hover tint so dragging the cursor down the list glides instead
        // of flashing each row on and off.
        float& hover = hoverAmountFor(e.mod);
        hover = MathUtils::animate(hovered ? 1.f : 0.f, hover, std::clamp(dt * 14.f, 0.f, 1.f));

        if (hovered && clicked) {
            e.mod->Toggle();
            if (Module* ts = ModuleManager::GetInstance().GetModuleByName("ToggleSounds")) {
                if (ts->IsEnabled()) ToggleSounds::PlayToggleSound(e.mod->IsEnabled());
            }
            // Just the module NAME + type (Nova derives the status from it).
            Notifications::AddNotification(
                e.mod->GetName(),
                e.mod->IsEnabled() ? NotifType::ToggleOn : NotifType::ToggleOff);
        }

        const ImColor accent = colorAt(gradientBase + slot.top * 0.15f);

        paintSurface(f, slot, hover, dl);
        paintText(f, e, slot, accent, gradientBase, dl);

        gradientBase += e.name.length() * 6.f + 28.f;
    }
}
